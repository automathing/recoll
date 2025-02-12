#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# dia (http://live.gnome.org/Dia) file filter for recoll
# stefan.friedel@iwr.uni-heidelberg.de 2012
#
# add the following to ~/.recoll/mimeconf into the [index] section:
# application/x-dia-diagram = execm rcldia.py;mimetype=text/plain;charset=utf-8
# and into the [icons] section:
# application/x-dia-diagram = drawing
# and finally under [categories]:
# other = ...\
#       application/x-dia-diagram
#
# in ~/.recoll/mimemap:
# .dia = application/x-dia-diagram

# Small fixes from jfd: dia files are sometimes not compressed.
import rclexecm
from rclbasehandler import RclBaseHandler
import re
from gzip import GzipFile
import xml.parsers.expat

# some regexps to parse/format the xml data: delete #/spaces at the b/eol and
# ignore empty lines
rhs = re.compile(r"^#\s*(.*)")
rhe = re.compile(r"(.*)\s*#$")
rempty = re.compile(r"^#?\s*#?$")


# xml parser for dia xml file
class Parser:
    def __init__(self, rclem):
        self._parser = xml.parsers.expat.ParserCreate(encoding="UTF-8")
        self._parser.StartElementHandler = self.startelement
        self._parser.EndElementHandler = self.endelement
        self._parser.CharacterDataHandler = self.chardata
        self.string = []
        self.handlethis = False
        self.rclem = rclem

    def startelement(self, name, attrs):
        if name == "dia:string":
            self.handlethis = True
        else:
            self.handlethis = False

    def chardata(self, data):
        if self.handlethis:
            # check if line is not empty and replace hashes/spaces
            if not rempty.search(data):
                self.string.append(rhe.sub(r"\1", rhs.sub(r"\1", data)))

    def endelement(self, name):
        self.handlethis = False

    def feed(self, fh):
        self._parser.ParseFile(fh)
        del self._parser


class DiaExtractor(RclBaseHandler):

    def __init__(self, em):
        super(DiaExtractor, self).__init__(em)

    def html_text(self, fn):
        try:
            dia = GzipFile(fn, "rb")
            # Dia files are sometimes not compressed. Quite weirdly,
            # GzipFile does not complain until we try to read.
            data = dia.readline()
            dia.seek(0)
        except:
            # File not compressed ?
            dia = open(fn, "rb")

        diap = Parser(self.em)
        diap.feed(dia)

        html = "<html><head><title></title></head><body><pre>"
        html += rclexecm.htmlescape("\n".join(diap.string))
        html += "</pre></body></html>"

        return html


# Main program: create protocol handler and extractor and run them
proto = rclexecm.RclExecM()
extract = DiaExtractor(proto)
rclexecm.main(proto, extract)
