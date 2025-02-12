#!/usr/bin/env python3
"""Extract Html files from a Microsoft Compiled Html Help file (.chm)"""

import sys
import os
import re
import posixpath
from urllib.parse import unquote as urllib_unquote
from urllib.parse import urlparse as urlparse_urlparse
from html.parser import HTMLParser
import subprocess

import rclconfig
import rclexecm

# pychm has no official port to Python3, hence no package in the
# standard place. Linux Recoll bundles a python3 port which is identical
# to pychm, but named recollchm to avoid conflicts because it is installed
# as a normal python package (in /usr/lib/pythonxx/dist-packages,
# not recoll/filters.). No such issues on Windows
try:
    # First try the system (or recoll-local on Windows) version if any
    from chm import chm, chmlib
except:
    try:
        from recollchm import chm, chmlib
    except:
        print("RECFILTERROR HELPERNOTFOUND python3:chm")
        sys.exit(1)


def _deb(s):
    print("%s" % s, file=sys.stderr)


# Small helper routines
def getfile(chmfile, path):
    """Extract internal file text from chm object, given path"""
    if type(path) != type(b""):
        raise Exception("Chm:getfile: must be called with path as bytes")
    res, ui = chmfile.ResolveObject(path)
    if res != chmlib.CHM_RESOLVE_SUCCESS:
        # _deb("ResolveObject failed: %s" % path)
        return ""
    res, doc = chmfile.RetrieveObject(ui)
    if not res:
        _deb("RetrieveObject failed: %s" % path)
        return ""
    return doc


def peekfile(chmfile, path, charset):
    """Check that path resolves in chm object"""
    if type(path) == type(""):
        path = path.encode(charset)
    res, ui = chmfile.ResolveObject(path)
    if res != chmlib.CHM_RESOLVE_SUCCESS:
        return False
    return True


# CHM Topics tree handler


class ChmTopicsParser(HTMLParser):
    """Parse the chm's Topic file which is basically
    a listing of internal nodes (html files mostly). Build a list of
    all nodes (parent.contents), which will then be used to walk and index
    the chm.

    Most nodes in the Topic file look like the following:
    <LI> <OBJECT type="text/sitemap">
           <param name="Name" value="Global Module Index">
           <param name="Local" value="modindex.html">
          </OBJECT>

    Maybe we should filter out non "text/sitemap" Objects, and maybe there are
    things of interest whose name is not Local, but for now, we just take
    all values for parameters named "Local" (with some filtering/massaging),
    until proven wrong
    """

    def __init__(self, rclchm):
        HTMLParser.__init__(self)
        self.em = rclchm.em
        self.rclchm = rclchm

    def handle_starttag(self, tag, attrs):
        # self.em.rclog("Beginning of a %s tag" % tag)
        # If this is a param tag with name Local, we're interested in
        # the value which lists a file ref. Discard those with #
        # in them (references inside files)
        # Sometimes it seems that refs are like Vendor:filename::path,
        # we only keep the path, and only if the file matches

        if tag != "param":
            return

        name = ""
        value = ""
        for nm, val in attrs:
            if nm == "name":
                name = val
            if nm == "value":
                value = val

        # self.em.rclog("Name [%s] value [%s]" %(name, value))

        if name != "Local" or value == "":
            return
        # value may be url-encoded. Decode it. If there are no % in there, will
        # do nothing
        value = urllib_unquote(value)

        localpath = ""
        ll = value.split(":")
        if len(ll) == 1:
            localpath = value
        elif len(ll) == 4 and ll[-1] and ll[-3]:
            # self.em.rclog("File: [%s] sfn [%s]" % ((ll[-3]), self.rclchm.sfn))
            # We used to test against the simple file name, but this does
            # not work if the file is renamed. Just check that the internal
            # path resolves. Old: if ll[-3] == self.rclchm.sfn:
            localpath = ll[-1]
            if not peekfile(self.rclchm.chm, localpath, self.rclchm.charset):
                # self.em.rclog("SKIPPING %s" % ll[-3])
                localpath = ""

        if len(localpath) != 0 and localpath.find("#") == -1:
            if localpath[0] != "/":
                localpath = "/" + localpath
            self.rclchm.contents.append(localpath.encode(self.rclchm.charset))


# Used when there is no Topics node. Walk the links tree
class ChmWalker(HTMLParser):
    """Links tree walker. This recursively follows all internal links
    found in the tree from the top node given as input, and augments
    the contents list."""

    def __init__(self, rclchm, path, contents):
        HTMLParser.__init__(self)
        self.rclchm = rclchm
        self.chm = rclchm.chm
        self.contents = contents
        if type(path) == type(""):
            path = path.encode(self.rclchm.charset)
        self.path = posixpath.normpath(path)
        self.dir = posixpath.dirname(self.path)
        contents.append(self.path)

    def handle_starttag(self, tag, attrs):
        if tag != "a":
            return

        href = ""
        for nm, val in attrs:
            if nm == "href":
                href = val

        path = ""
        res = urlparse_urlparse(href)
        if not res.scheme or res.scheme.lower == "ms-its":
            path = res.path
            lpath = path.split(":")
            if len(lpath) == 3:
                # MS-ITS::somefile.chm:/some/path/file.htm ? As far as I
                # know this never happens because there was a runtime error
                # in this path
                path = lpath[2]
                if not peekfile(self.chm, path, self.rclchm.charset):
                    path = ""
            elif len(lpath) == 1:
                path = lpath[0]
            else:
                path = ""

        if path:
            bpath = path.encode(self.rclchm.charset)
            if path[0] == "/"[0]:
                npath = posixpath.normpath(bpath)
            else:
                npath = posixpath.normpath(posixpath.join(self.dir, bpath))
            if not npath in self.contents:
                # _deb("Going into [%s] paths [%s]\n" % (npath,str(self.contents)))
                text = getfile(self.chm, npath)
                if text:
                    try:
                        newwalker = ChmWalker(self.rclchm, npath, self.contents)
                        t, c = self.rclchm.fixencoding(text)
                        newwalker.feed(t)
                    except:
                        pass


class rclCHM:
    """RclExecM slave worker for extracting all files from an Msoft chm
    file. We first extract the list of internal nodes, and them return them
    one by one. The ipath is the node path"""

    def __init__(self, em):
        self.contents = []
        self.chm = chm.CHMFile()
        self.em = em
        cf = rclconfig.RclConfig()
        self.catenate = cf.getConfParam("chmcatenate")
        self.catenate = int(self.catenate) if self.catenate else False
        self.em.setmimetype("text/html")
        expr = rb'(<meta *http-equiv *= *"content-type".*charset *= *)((us-)?ascii)( *" *>)'
        self.asciito1252re = re.compile(expr, re.IGNORECASE)
        expr = rb'<meta *http-equiv *= *"content-type".*charset *= *([a-z0-9-]+) *" *>'
        self.findcharsetre = re.compile(expr, re.IGNORECASE)
        self._headtagre = re.compile(rb"</head>", re.IGNORECASE)
        self._headerre = re.compile(rb"(<head.*</head>)", re.IGNORECASE | re.DOTALL)
        self._bodyre = re.compile(rb"<body[^>]*>(.*)</body>", re.IGNORECASE | re.DOTALL)

    def extractone(self, path, norclaptag=False):
        """Extract one path-named internal file from the chm file"""

        # self.em.rclog("extractone: [%s]" % (path,))
        if type(path) == type(""):
            path = path.encode(self.charset)
        iseof = rclexecm.RclExecM.noteof
        if self.currentindex >= len(self.contents) - 1:
            iseof = rclexecm.RclExecM.eofnext

        res, ui = self.chm.ResolveObject(path)
        # self.em.rclog("extract: ResolveO: %d [%s]" % (res, ui))
        if res != chmlib.CHM_RESOLVE_SUCCESS:
            return (False, "", path, iseof)
        # RetrieveObject() returns len,value
        res, doc = self.chm.RetrieveObject(ui)
        # self.em.rclog("extract: RetrieveObject: %d [%s]" % (res, doc))
        if res > 0:
            if not norclaptag:
                doc = self._headtagre.sub(
                    b'<meta name="rclaptg" content="chm"></head>', doc
                )
            return (True, doc, path, iseof)
        return (False, "", path, iseof)

    def dumpall(self):
        alltxt = b""
        first = True
        for pth in self.contents:
            ret, doc, path, iseof = self.extractone(pth, norclaptag=True)
            if not ret:
                continue
            if first:
                # Save a header
                headmatch = self._headerre.search(doc)
                if headmatch:
                    header = headmatch[1]
                    # _deb("HEADER [%s]" % header)
                    # _deb("type(self.chm.title) %s" % type(self.chm.title))
                    if type(self.chm.title) == type(""):
                        title = self.chm.title.encode(self.charset)
                    else:
                        title = self.chm.title
                    header = re.sub(
                        rb"<title.*</title>",
                        b"<title>" + title + b"</title>",
                        doc,
                        re.IGNORECASE | re.DOTALL,
                    )
                    first = False
                    alltxt += header + b"<body>"
            body = self._bodyre.search(doc)
            if body:
                body = body[1]
                # _deb("BODY [%s]" % body[0:200])
                alltxt += body
        alltxt += b"</body></html>"
        return alltxt

    def fixencoding(self, text):
        """Fix encoding for supposedly html document. We do 2 things here:
        - Change any 'ASCII' charset decl to windows-1252
        - Decode the string if it's originally bytes because
          that's what Python HTMLParser actually expects even if it does not
          really say so. See http://bugs.python.org/issue3932.
        """

        # Memo. Charset decl example. Maybe we should also process the
        # HTML5 charset tag ?
        # <META HTTP-EQUIV="Content-Type" CONTENT="text/html; charset=US-ASCII">

        if type(text) == type(b""):
            # Fix an ascii charset decl to windows-1252
            text = self.asciito1252re.sub(rb"\1windows-1252\4", text, 1)
            # Convert to unicode according to charset decl
            m = self.findcharsetre.search(text)
            if m:
                charset = m.group(1).decode("cp1252")
            else:
                charset = "cp1252"
            text = text.decode(charset, errors="replace")
        return text, charset

    def closefile(self):
        self.chm.CloseCHM()

    def openfile(self, params):
        """Open the chm file and build the contents list by extracting and
        parsing the Topics object"""

        self.currentindex = -1
        self.contents = []

        filename = params["filename"]
        if not self.chm.LoadCHM(filename):
            self.em.rclog("LoadCHM failed")
            return False

        # self.em.rclog("home [%s] topics [%s] title [%s]" %
        #              (self.chm.home, self.chm.topics, self.chm.title))

        self.topics = self.chm.GetTopicsTree()
        self.charset = "cp1252"
        if self.topics:
            # Parse Topics file and extract list of internal nodes
            # self.em.rclog("Got topics");
            tp = ChmTopicsParser(self)
            text, self.charset = self.fixencoding(self.topics)
            tp.feed(text)
            tp.close()
        else:
            # No topics. If there is a home, let's try to walk the tree
            # self.em.rclog("GetTopicsTree failed")
            if not self.chm.home:
                self.em.rclog("No topics and no home")
                return False
            home = self.chm.home
            if home[0] != b"/"[0]:
                home = b"/" + home
            text = getfile(self.chm, home)
            if not text:
                self.em.rclog("No topics and no home content")
                return False
            walker = ChmWalker(self, self.chm.home, self.contents)
            text, self.charset = self.fixencoding(text)
            walker.feed(text)
            walker.close()

        # Eliminate duplicates but keep order (can't directly use set)
        u = set()
        ct = []
        for t in self.contents:
            if t not in u:
                ct.append(t)
                u.add(t)
        self.contents = ct
        # self.em.rclog("Contents size %d contents %s" % (len(self.contents), self.contents))
        return True

    def getipath(self, params):
        return self.extractone(params["ipath"])

    def getnext(self, params):
        if self.catenate:
            alltxt = self.dumpall()
            self.closefile()
            if alltxt:
                return (True, alltxt, "", rclexecm.RclExecM.eofnext)
            else:
                return (False, "", "", rclexecm.RclExecM.eofnow)

        if self.currentindex == -1:
            # Return "self" doc
            self.currentindex = 0
            self.em.setmimetype("text/plain")
            if len(self.contents) == 0:
                self.closefile()
                eof = rclexecm.RclExecM.eofnext
            else:
                eof = rclexecm.RclExecM.noteof
            return (True, "", "", eof)

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
extract = rclCHM(proto)
rclexecm.main(proto, extract)
