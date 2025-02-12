#!/usr/bin/env python3

# WAR web archive filter for recoll. War file are gzipped tar files

import rclexecm
import tarfile


class WarExtractor:
    def __init__(self, em):
        self.em = em

    def extractone(self, tarinfo):
        docdata = ""
        iseof = rclexecm.RclExecM.noteof
        try:
            member = self.tar.extractfile(tarinfo)
            docdata = member.read()
            ok = True
        except Exception as err:
            self.em.rclog("extractone: failed: [%s]" % err)
            iseof = rclexecm.RclExecM.eofnow
            ok = False
        return (ok, docdata, tarinfo.name, iseof)

    def closefile(self):
        self.tar = None

    ###### File type handler api, used by rclexecm ---------->
    def openfile(self, params):
        self.currentindex = -1
        try:
            self.tar = tarfile.open(params["filename"])
            return True
        except Exception as err:
            self.em.rclog(str(err))
            return False

    def getipath(self, params):
        ipath = params["ipath"]
        try:
            tarinfo = self.tar.getmember(ipath)
        except Exception as err:
            self.em.rclog(str(err))
            return (False, "", ipath, rclexecm.RclExecM.noteof)
        return self.extractone(tarinfo)

    def getnext(self, params):
        if self.currentindex == -1:
            # Return "self" doc
            self.currentindex = 0
            return (True, "", "", rclexecm.RclExecM.noteof)

        tarinfo = self.tar.next()
        if tarinfo is None:
            # self.em.rclog("getnext: EOF hit")
            self.closefile()
            return (False, "", "", rclexecm.RclExecM.eofnow)
        else:
            ret = self.extractone(tarinfo)
            if (
                ret[3] == rclexecm.RclExecM.eofnext
                or ret[3] == rclexecm.RclExecM.eofnow
            ):
                self.closefile()
            return ret


# Main program: create protocol handler and extractor and run them
proto = rclexecm.RclExecM()
extract = WarExtractor(proto)
rclexecm.main(proto, extract)
