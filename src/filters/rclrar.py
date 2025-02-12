#!/usr/bin/env python3

# Rar file filter for Recoll
# Adapted from the Zip archive filter by mroark.
# Copyright (C) 2004 J.F.Dockes + mroark for rar bits
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

import sys
import os

import rclexecm
from archivextract import ArchiveExtractor


# We can use two different unrar python modules. Either python3-rarfile
# which is a wrapper over the the unrar command line, or python3-unrar
# which is a ctypes wrapper of the unrar lib.
#
# Python3-rarfile is the one commonly packaged on linux.
#
# Python3-unrar needs the libunrar library, built from unrar source
# code (https://www.rarlab.com/rar_add.htm), which is not packaged on
# Debian or Fedora, probably because of licensing issues. It is
# packaged as libunrar5 on Ubuntu, and there are Fedora packages on
# RPM Fusion, and it's probably easy to build the Ubuntu package on
# Debian.  Python-unrar works much better and is the right choice if
# the licensing is not an issue.
#
# The interfaces is similar, but python-unrar uses forward slashes
# in internal file paths while python-rarfile uses backslashes (ipaths
# are opaque anyway).

using_unrar = False
try:
    from unrar import rarfile

    using_unrar = True
except Exception as ex:
    try:
        from rarfile import RarFile
    except:
        print("RECFILTERROR HELPERNOTFOUND python3:rarfile/python3:unrar")
        sys.exit(1)


# Requires RarFile python module. Try "sudo pip install rarfile" or
# install it with the system package manager
#
# Also for many files, you will need the non-free version of unrar
# (https://www.rarlab.com/rar_add.htm). The unrar-free version fails
# with the message "Failed the read enough data"
#
# This is identical to rclzip.py except I did a search/replace from zip
# to rar, and changed this comment.
class RarExtractor(ArchiveExtractor):
    def __init__(self, em):
        super().__init__(em)

    def extractone(self, ipath):
        # self.em.rclog("extractone: [%s]" % ipath)
        docdata = ""
        isdir = False

        try:
            if using_unrar:
                if type(ipath) == type(b""):
                    ipath = ipath.decode("UTF-8")
                rarinfo = self.rar.getinfo(ipath)
                # dll.hpp RHDF_DIRECTORY: 0x20
                isdir = (rarinfo.flag_bits & 0x20) != 0
            else:
                rarinfo = self.rar.getinfo(ipath)
                isdir = rarinfo.isdir()
        except Exception as err:
            self.em.rclog(
                "extractone: using_unrar %d rar.getinfo failed: [%s]"
                % (using_unrar, err)
            )
            return (True, docdata, ipath, False)

        if not isdir:
            try:
                if rarinfo.file_size > self.em.maxmembersize:
                    self.em.rclog(
                        "extractone: entry %s size %d too big"
                        % (ipath, rarinfo.file_size)
                    )
                    docdata = ""
                else:
                    docdata = self.rar.read(ipath)
                ok = True
            except Exception as err:
                self.em.rclog("extractone: rar.read failed: [%s]" % err)
                ok = False
        else:
            docdata = ""
            ok = True
            self.em.setmimetype("application/x-fsdirectory")

        iseof = rclexecm.RclExecM.noteof
        if self.currentindex >= len(self.rar.namelist()) - 1:
            iseof = rclexecm.RclExecM.eofnext
        return (ok, docdata, rclexecm.makebytes(ipath), iseof)

    def closefile(self):
        self.rar = None

    ###### File type handler api, used by rclexecm ---------->
    def openfile(self, params):
        self.currentindex = -1
        filename = params["filename"]
        self.namefilter.setforlocation(filename)
        try:
            if using_unrar:
                # There might be a way to avoid the decoding which is
                # wrong on Unix, but I'd have to dig further in the
                # lib than I wish to. This is used on Windows anyway,
                # where all Recoll paths are utf-8
                fn = filename.decode("UTF-8")
                self.rar = rarfile.RarFile(fn, "rb")
            else:
                # The previous versions passed the file name to
                # RarFile. But the py3 version of this wants an str as
                # input, which is wrong of course, as filenames are
                # binary. Circumvented by passing the open file
                f = open(filename, "rb")
                self.rar = RarFile(f)
            return True
        except Exception as err:
            self.em.rclog("RarFile: %s" % err)
            return False

    def namelist(self):
        return self.rar.namelist()

    # getipath from ArchiveExtractor
    # getnext from ArchiveExtractor


# Main program: create protocol handler and extractor and run them
proto = rclexecm.RclExecM()
extract = RarExtractor(proto)
rclexecm.main(proto, extract)
