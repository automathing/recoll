#!/usr/bin/env python3

# Read a file in GNU info format and output its nodes as subdocs,
# interfacing with recoll execm

import rclexecm
import sys
import os
import subprocess

# Prototype for the html document we're returning. Info files are
# normally ascii. Set no charset, and let it be provided by the
# environment if necessary
#
# Some info source docs contain charset info like:
# @documentencoding ISO-2022-JP
# But this seems to be absent from outputs.


# RclExecm interface
class InfoExtractor:
    def __init__(self, em):
        self.file = ""
        self.contents = []
        self.em = em

    def extractone(self, index):
        if index >= len(self.contents):
            return (False, "", "", True)

        nodename, docdata = self.contents[index]
        nodename = rclexecm.htmlescape(nodename)
        docdata = rclexecm.htmlescape(docdata)
        # strange whitespace to avoid changing the module tests (same as old)
        docdata = (
            b"\n<html>\n  <head>\n      <title>"
            + nodename
            + b"</title>\n"
            + b'      <meta name="rclaptg" content="gnuinfo">\n'
            + b"   </head>\n   <body>\n"
            + b'   <pre style="white-space: pre-wrap">\n   '
            + docdata
            + b"\n   </pre></body>\n</html>\n"
        )

        iseof = rclexecm.RclExecM.noteof
        if self.currentindex >= len(self.contents) - 1:
            iseof = rclexecm.RclExecM.eofnext
        self.em.setmimetype("text/html")
        return (True, docdata, str(index), iseof)

    ###### File type handler api, used by rclexecm ---------->
    def openfile(self, params):
        self.file = params["filename"]

        if not os.path.isfile(self.file):
            self.em.rclog("Openfile: %s is not a file" % self.file)
            return False

        cmd = b"info --subnodes -o - -f " + self.file
        nullstream = open(os.devnull, "w")
        try:
            infostream = subprocess.Popen(
                cmd, shell=True, bufsize=1, stderr=nullstream, stdout=subprocess.PIPE
            ).stdout
        except Exception as e:
            # Consider this as permanently fatal.
            self.em.rclog("Openfile: exec info: %s" % str(e))
            print("RECFILTERROR HELPERNOTFOUND info")
            sys.exit(1)

        self.currentindex = -1

        self.contents = InfoSimpleSplitter().splitinfo(self.file, infostream)

        # self.em.rclog("openfile: Entry count: %d"%(len(self.contents)))
        return True

    # Extract specific node
    def getipath(self, params):
        try:
            index = int(params["ipath"])
        except:
            return (False, "", "", True)
        return self.extractone(index)

    # Extract next in list
    def getnext(self, params):

        if self.currentindex == -1:
            # Return "self" doc
            self.currentindex = 0
            self.em.setmimetype("text/plain")
            if len(self.contents) == 0:
                eof = rclexecm.RclExecM.eofnext
            else:
                eof = rclexecm.RclExecM.noteof
            return (True, "", "", eof)

        if self.currentindex >= len(self.contents):
            self.em.rclog("getnext: EOF hit")
            return (False, "", "", rclexecm.RclExecM.eofnow)
        else:
            ret = self.extractone(self.currentindex)
            self.currentindex += 1
            return ret


# Info file splitter
class InfoSimpleSplitter:

    def splitinfo(self, filename, fin):
        gotblankline = 1
        index = 0
        listout = []
        node_dict = {}
        node = b""
        infofile = os.path.basename(filename)
        nodename = b"Unknown"

        for line in fin:

            # Top of node ?
            # It sometimes happens that info --subnodes produces a Node line
            # beginning with spaces (it's a bug probably, only seen it once)
            # Maybe we'd actually be better off directly interpreting the
            # info files
            if gotblankline and line.lstrip(b" ").startswith(b"File: "):
                prevnodename = nodename
                line = line.rstrip(b"\n\r")
                pairs = line.split(b",")
                up = b"Top"
                nodename = str(index)
                try:
                    for pair in pairs:
                        name, value = pair.split(b":")
                        name = name.strip(b" ")
                        value = value.strip(b" ")
                        if name == b"Node":
                            nodename = value
                        if name == b"Up":
                            up = value
                        if name == b"File":
                            infofile = value
                except Exception as err:
                    print(
                        "rclinfo.py: bad line in %s: [%s] %s\n" % (infofile, line, err),
                        file=sys.stderr,
                    )
                    nodename = prevnodename
                    node += line
                    continue

                if nodename in node_dict:
                    print(
                        "Info file %s Dup node: %s" % (filename, nodename),
                        file=sys.stderr,
                    )
                node_dict[nodename] = up

                if index != 0:
                    listout.append((prevnodename, node))
                node = b""
                index += 1

            if line.rstrip(b"\n\r") == b"":
                gotblankline = 1
            else:
                gotblankline = 0

            node += line

        # File done, add last dangling node
        if node != b"":
            listout.append((nodename, node))

        # Compute node paths (concatenate "Up" values), to be used
        # as page titles. It's unfortunate that this will crash if
        # the info file tree is bad
        listout1 = []
        for nodename, node in listout:
            title = b""
            loop = 0
            error = 0
            while nodename != b"Top":
                title = nodename + b" / " + title
                if nodename in node_dict:
                    nodename = node_dict[nodename]
                else:
                    print(
                        "Infofile: node's Up does not exist: file %s, path %s, up [%s]"
                        % (infofile, title, nodename),
                        sys.stderr,
                    )
                    error = 1
                    break
                loop += 1
                if loop > 50:
                    print("Infofile: bad tree (looping) %s" % infofile, file=sys.stderr)
                    error = 1
                    break

            if error:
                continue

            if title == b"":
                title = infofile
            else:
                title = infofile + b" / " + title
            title = title.rstrip(b" / ")
            listout1.append((title, node))

        return listout1


##### Main program: either talk to the parent or execute test loop
proto = rclexecm.RclExecM()
extract = InfoExtractor(proto)
rclexecm.main(proto, extract)
