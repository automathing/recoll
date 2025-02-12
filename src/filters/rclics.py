#!/usr/bin/env python3

# Read an ICS file, break it into "documents" which are events, todos,
# or journal entries, and interface with recoll execm
#
# For historical reasons, this can use either the icalendar or the
# vobject Python modules, or an internal splitter. The default is now
# to use the internal splitter, the other modules are more trouble
# than they're worth (to us and until we will want to get into date
# computations etc.)

import rclexecm
import sys

# Decide how we'll process the file.
modules = ("internal", "icalendar", "vobject")
usemodule = "internal"
forcevobject = 0
if usemodule != "internal":
    try:
        if forcevobject:
            raise Exception
        from icalendar import Calendar, Event

        usemodule = "icalendar"
    except:
        try:
            import vobject

            usemodule = "vobject"
        except:
            print("RECFILTERROR HELPERNOTFOUND python3:icalendar")
            print("RECFILTERROR HELPERNOTFOUND python3:vobject")
            sys.exit(1)


class IcalExtractor:
    def __init__(self, em):
        self.file = ""
        self.contents = []
        self.em = em

    def extractone(self, index):
        if index >= len(self.contents):
            return (False, "", "", True)
        docdata = self.contents[index]
        # self.em.rclog(docdata)

        iseof = rclexecm.RclExecM.noteof
        if self.currentindex >= len(self.contents) - 1:
            iseof = rclexecm.RclExecM.eofnext
        self.em.setmimetype("text/plain")
        return (True, docdata, str(index), iseof)

    ###### File type handler api, used by rclexecm ---------->
    def openfile(self, params):
        self.file = params["filename"]

        try:
            calstr = open(self.file, "rb")
        except Exception as e:
            self.em.rclog("Openfile: open: %s" % str(e))
            return False

        self.currentindex = -1

        if usemodule == "internal":
            self.contents = ICalSimpleSplitter().splitcalendar(calstr)
        elif usemodule == "icalendar":
            try:
                cal = Calendar.from_string(calstr.read())
            except Exception as e:
                self.em.rclog("Openfile: read or parse error: %s" % str(e))
                return False
            self.contents = cal.walk()
            self.contents = [
                item.as_string()
                for item in self.contents
                if (
                    item.name == "VEVENT"
                    or item.name == "VTODO"
                    or item.name == "VJOURNAL"
                )
            ]
        else:
            try:
                cal = vobject.readOne(calstr)
            except Exception as e:
                self.em.rclog("Openfile: can't parse object: %s" % str(e))
                return False
            for lstnm in ("vevent_list", "vtodo_list", "vjournal_list"):
                lst = getattr(cal, lstnm, [])
                for ev in lst:
                    self.contents.append(ev.serialize())

        # self.em.rclog("openfile: Entry count: %d"%(len(self.contents)))
        return True

    def getipath(self, params):
        try:
            if params["ipath"] == b"":
                index = 0
            else:
                index = int(params["ipath"])
        except:
            return (False, "", "", True)
        return self.extractone(index)

    def getnext(self, params):

        if self.currentindex == -1:
            # Return "self" doc
            self.currentindex = 0
            self.em.setmimetype(b"text/plain")
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


# Trivial splitter: cut objects on BEGIN/END (only for 'interesting' objects)
# ignore all other syntax
class ICalSimpleSplitter:
    # Note that if an 'interesting' element is nested inside another one,
    # it will not be extracted (stay as text in external event). This is
    # not an issue and I don't think it can happen with the current list
    interesting = (b"VTODO", b"VEVENT", b"VJOURNAL")

    def splitcalendar(self, fin):
        curblkname = b""
        curblk = b""

        lo = []
        for line in fin:
            line = line.rstrip()
            if line == b"":
                continue

            if curblkname:
                curblk = curblk + line + b"\n"

            l = line.split(b":")
            if len(l) < 2:
                continue

            # If not currently inside a block and we see an
            # 'interesting' BEGIN, start block
            if curblkname == b"" and l[0].upper() == b"BEGIN":
                name = l[1].upper()
                if name in ICalSimpleSplitter.interesting:
                    curblkname = name
                    curblk = curblk + line + b"\n"

            # If currently accumulating block lines, check for end
            if curblkname and l[0].upper() == b"END" and l[1].upper() == curblkname:
                lo.append(curblk)
                curblkname = b""
                curblk = b""

        if curblk:
            lo.append(curblk)
            curblkname = b""
            curblk = b""

        return lo


proto = rclexecm.RclExecM()
extract = IcalExtractor(proto)
rclexecm.main(proto, extract)
