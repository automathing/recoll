#!/usr/bin/env python3
"""Extract Html content from an EPUB file (.epub)"""

rclepub_html_mtype = "text/html"

import sys
import os
import re
import subprocess

import rclexecm
import rclconfig

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "recollepub.zip"))
try:
    import epub
except:
    print("RECFILTERROR HELPERNOTFOUND python3:epub")
    sys.exit(1)


class rclEPUB:
    """RclExecM slave worker for extracting all text from an EPUB
    file. We first extract the list of internal nodes, and them return them
    one by one. The ipath is the internal href"""

    def __init__(self, em):
        self.currentindex = 0
        self.em = em
        self.em.setmimetype(rclepub_html_mtype)
        cf = rclconfig.RclConfig()
        self.catenate = cf.getConfParam("epubcatenate")
        self.catenate = int(self.catenate) if self.catenate else False

    def _docheader(self):
        meta = self.book.opf.metadata
        title = ""
        for tt, lang in meta.titles:
            title += tt + " "
        author = ""
        for name, role, fileas in meta.creators:
            author += name + " "
        data = "<html>\n<head>\n"
        if title:
            data += "<title>" + rclexecm.htmlescape(title) + "</title>\n"
        if author:
            data += (
                '<meta name="author" content="'
                + rclexecm.htmlescape(author).strip()
                + '">\n'
            )
        if meta.description:
            data += (
                '<meta name="description" content="'
                + rclexecm.htmlescape(meta.description)
                + '">\n'
            )
        for value in meta.subjects:
            data += (
                '<meta name="dc:subject" content="'
                + rclexecm.htmlescape(value)
                + '">\n'
            )
        data += "</head>"
        return data.encode("UTF-8")

    def _catbodies(self):
        data = b"<body>"
        ids = []
        if self.book.opf.spine:
            for id, linear in self.book.opf.spine.itemrefs:
                ids.append(id)
        else:
            for id, item in self.book.opf.manifest.items():
                ids.append(id)

        for id in ids:
            item = self.book.get_item(id)
            if item is None or item.media_type != "application/xhtml+xml":
                continue
            doc = self.book.read_item(item)
            doc = re.sub(rb"<\?.*\?>", b"", doc)
            doc = re.sub(rb"<html.*<body[^>]*>", b"", doc, 1, flags=re.DOTALL | re.I)
            doc = re.sub(rb"</body>", b"", doc, flags=re.I)
            doc = re.sub(rb"</html>", b"", doc, flags=re.I)
            data += doc

        data += b"</body></html>"
        return data

    def _selfdoc(self):
        data = self._docheader()
        self.em.setmimetype("text/html")
        if len(self.contents) == 0:
            self.closefile()
            eof = rclexecm.RclExecM.eofnext
        else:
            eof = rclexecm.RclExecM.noteof
        return (True, data, "", eof)

    def extractone(self, id):
        """Extract one path-named internal file from the EPUB file"""

        # self.em.rclog("extractone: [%s]"%(path))
        iseof = rclexecm.RclExecM.noteof
        if self.currentindex >= len(self.contents) - 1:
            iseof = rclexecm.RclExecM.eofnext

        try:
            item = self.book.get_item(id)
            if item is None:
                raise Exception("Item not found for id %s" % (id,))
            doc = self.book.read_item(item)
            doc = re.sub(
                rb"</[hH][eE][aA][dD]>",
                b'<meta name="rclaptg" content="epub"></head>',
                doc,
            )
            self.em.setmimetype(rclepub_html_mtype)
            return (True, doc, id, iseof)
        except Exception as err:
            self.em.rclog("extractone: failed: [%s]" % err)
            return (False, "", id, iseof)

    def dumpall(self):
        data = self._docheader()
        data += self._catbodies()
        return data

    def closefile(self):
        self.book.close()

    def openfile(self, params):
        """Open the EPUB file, create a contents array"""
        self.currentindex = -1
        self.contents = []
        try:
            self.book = epub.open_epub(params["filename"].decode("UTF-8"))
        except Exception as err:
            self.em.rclog("openfile: epub.open failed: [%s]" % err)
            return False
        for id, item in self.book.opf.manifest.items():
            if item.media_type == "application/xhtml+xml":
                self.contents.append(id)
        return True

    def getipath(self, params):
        return self.extractone(params["ipath"].decode("UTF-8"))

    def getnext(self, params):
        if self.catenate:
            alltxt = self.dumpall()
            self.closefile()
            if alltxt:
                return (True, alltxt, "", rclexecm.RclExecM.eofnext)
            else:
                return (False, "", "", rclexecm.RclExecM.eofnow)

        if self.currentindex == -1:
            self.currentindex = 0
            return self._selfdoc()

        if self.currentindex >= len(self.contents):
            self.closefile()
            return (False, "", "", rclexecm.RclExecM.eofnow)
        else:
            ret = self.extractone(self.contents[self.currentindex])
            if (
                ret[3] == rclexecm.RclExecM.eofnext
                or ret[3] == rclexecm.RclExecM.eofnow
            ):
                self.closefile()
            self.currentindex += 1
            return ret


proto = rclexecm.RclExecM()
extract = rclEPUB(proto)
rclexecm.main(proto, extract)
