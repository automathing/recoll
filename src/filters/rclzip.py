#!/usr/bin/env python3
# Copyright (C) 2014-2024 J.F.Dockes
#   This program is free software; you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation; either version 2 of the License, or
#   (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program; if not, write to the
#   Free Software Foundation, Inc.,
#   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#

# Zip file extractor for Recoll

import os
import posixpath
import datetime

from zipfile import ZipFile

import rclexecm
from archivextract import ArchiveExtractor


# Note about file names (python 2.6. 2.7, don't know about 3.)
#
# There is a bit in zip entries to indicate if the filename is encoded
# as utf-8 or not. If the bit is set, zipfile decodes the file name
# and stores it in the catalog as an unicode object. Else it uses the
# binary string, which it decodes as CP437 (zip standard).
#
# When reading the file, the input file name is used by rclzip.py
# directly as an index into the catalog.
#
# When we send the file name data to the indexer, we have to serialize
# it as byte string, we can't pass unicode objects to and from. This
# means that we have to test if the name is unicode. If it is, we send
# the string encoded as utf-8. When reading, if the input is utf-8, we
# turn it to unicode and use this to access the zip member, else we
# use the binary string.
#
# In the case where an archive member name is a valid non-ascii utf-8
# string, but the flag is not set (which could probably happen if the
# archiver did not try to detect utf-8 file names), this will fail,
# because we'll convert back the utf-8 string to unicode and pass this
# to zipfile, but an utf-8 string, not a unicode object is actually in
# the catalog in this case, so the access will fail (will be seen at
# preview or open time). This does not affect ascii file names because
# the representation is the same anyway.
#
# To avoid this problem, we'd need to pass a separate bit of
# information indicating that encoding was performed, not just rely on
# the utf-8 validity test (ie have a 1st char switch), but this would be
# incompatible with existing indexes. Instead we try both ways...
#
# Also, some zip files contain file names which are not encoded as
# CP437 (Ex: EUC-KR which was the test case). Python produces garbage
# paths in this case (this does not affect the ipath validity, just
# the display), which is expected, but unzip succeeds in guessing the
# correct encoding, I have no idea how, but apparently the magic
# occurs in process.c:GetUnicodeData(), which succeeds in finding an
# utf-8 string which zipfile does not see (to be checked: was a quick look).
# Anyway: this is a python zipfile issue.
class ZipExtractor(ArchiveExtractor):
    def __init__(self, em):
        self.filename = None
        self.f = None
        self.zip = None
        super().__init__(em)

    def closefile(self):
        # self.em.rclog("Closing %s" % self.filename)
        if self.zip:
            self.zip.close()
        if self.f:
            self.f.close()
        self.f = None
        self.zip = None

    def extractone(self, ipath):
        # self.em.rclog("extractone: [%s]" % ipath)
        docdata = ""
        try:
            info = self.zip.getinfo(ipath)
            # There could be a 4GB Iso in the zip. We have to set a limit
            if info.file_size > self.em.maxmembersize:
                self.em.rclog(
                    "extractone: entry %s size %d too big" % (ipath, info.file_size)
                )
                docdata = ""
                # raise BadZipfile()
            else:
                docdata = self.zip.read(ipath)
            try:
                # We are assuming here that the zip uses forward slash
                # separators, which is not necessarily the case. At
                # worse, we'll get a wrong or no file name, which is
                # no big deal (the ipath is the important data
                # element).
                filename = posixpath.basename(ipath)
                self.em.setfield("filename", filename)
                dt = datetime.datetime(*info.date_time)
                self.em.setfield("modificationdate", str(int(dt.timestamp())))
            except:
                pass
            ok = True
        except Exception as err:
            ok = False
        iseof = rclexecm.RclExecM.noteof
        if self.currentindex >= len(self.zip.namelist()) - 1:
            self.closefile()
            iseof = rclexecm.RclExecM.eofnext
        return (ok, docdata, rclexecm.makebytes(ipath), iseof)

    ###### File type handler api, used by rclexecm ---------->
    def openfile(self, params):
        self.closefile()
        filename = params["filename"]
        self.filename = filename
        self.currentindex = -1
        self.namefilter.setforlocation(filename)
        try:
            # Note: py3 ZipFile wants an str file name, which
            # is wrong: file names are binary. But it accepts an
            # open file, and open() has no such restriction
            self.f = open(filename, "rb")
            self.zip = ZipFile(self.f)
            return True
        except Exception as err:
            self.em.rclog("openfile: failed: [%s]" % err)
            return False

    def namelist(self):
        return self.zip.namelist()

    # getipath from ArchiveExtractor
    # getnext inherited from ArchiveExtractor


# Main program: create protocol handler and extractor and run them
proto = rclexecm.RclExecM()
extract = ZipExtractor(proto)
rclexecm.main(proto, extract)
