#!/usr/bin/python3
# -*- coding: utf-8 -*-
"""A Python simplified equivalent of the command line query tool recollq
The input string is always interpreted as a query language string.
This could actually be useful for something after some customization
"""

import sys
import locale
from getopt import getopt

from recoll import recoll, rclextract

allmeta = (
    "title",
    "keywords",
    "abstract",
    "url",
    "mimetype",
    "mtime",
    "ipath",
    "fbytes",
    "dbytes",
    "relevancyrating",
)


def Usage():
    print("Usage: recollq.py [-c conf] [-i extra_index] <recoll query>")
    sys.exit(1)


class ptrmeths:
    def __init__(self, groups):
        self.groups = groups

    def startMatch(self, idx):
        ugroup = " ".join(self.groups[idx][1])
        return '<span class="pyrclstart" idx="%d" ugroup="%s">' % (idx, ugroup)

    def endMatch(self):
        return "</span>"


def extract(doc):
    extractor = rclextract.Extractor(doc)
    newdoc = extractor.textextract(doc.ipath)
    return newdoc


def extractofile(doc, outfilename=""):
    extractor = rclextract.Extractor(doc)
    outfilename = extractor.idoctofile(doc.ipath, doc.mimetype, ofilename=outfilename)
    return outfilename


def doquery(db, q):
    # Get query object
    query = db.query()
    # query.sortby("dmtime", ascending=True)

    # Parse/run input query string
    nres = query.execute(q, stemming=0, stemlang="english")
    qs = "Xapian query: [%s]" % query.getxquery()
    print(f"{qs}")
    groups = query.getgroups()
    m = ptrmeths(groups)

    # Print results:
    print("Result count: %d %d" % (nres, query.rowcount))
    if nres > 20:
        nres = 20
    # results = query.fetchmany(nres)
    # for doc in results:

    for i in range(nres):
        doc = query.fetchone()
        rownum = query.next if type(query.next) == int else query.rownumber
        print("%d:" % (rownum,))

        # for k,v in doc.items().items():
        #    print(f"KEY: {k} VALUE: {v}")
        # continue

        # outfile = extractofile(doc) ; print(f"outfile: {outfile} url: {doc.url}")

        for k in ("title", "mtime", "author"):
            value = getattr(doc, k)
            # value = doc.get(k)
            if value is None:
                print(f"{k}: (None)")
            else:
                print(f"{k} : {value}")
        # doc.setbinurl(bytearray("toto"))
        # burl = doc.getbinurl(); print("Bin URL : [%s]"%(doc.getbinurl(),))
        abs = query.makedocabstract(doc, methods=m)
        print(f"{abs}\n")


#        fulldoc = extract(doc)
#        print("FULLDOC MIMETYPE %s TEXT: %s" % (fulldoc.mimetype,fulldoc.text))


########################################### MAIN

if len(sys.argv) < 2:
    Usage()

language, localecharset = locale.getdefaultlocale()
confdir = ""
extra_dbs = []
# Snippet params
maxchars = 120
contextwords = 4
syngroupsfile = ""
# Process options: [-c confdir] [-i extra_db [-i extra_db] ...]
try:
    options, args = getopt(sys.argv[1:], "c:i:T:")
except Exception as ex:
    print(f"{ex}")
    sys.exit(1)
for opt, val in options:
    if opt == "-c":
        confdir = val
    elif opt == "-i":
        extra_dbs.append(val)
    elif opt == "-T":
        syngroupsfile = val
    else:
        print("Bad opt: %s" % (opt,))
        Usage()

# The query should be in the remaining arg(s)
if len(args) == 0:
    print("No query found in command line")
    Usage()
q = ""
for word in args:
    q += word + " "

print(f"QUERY: [{q}]")
db = recoll.connect(confdir=confdir, extra_dbs=extra_dbs)
db.setAbstractParams(maxchars=maxchars, contextwords=contextwords)
if syngroupsfile:
    db.setSynonymsFile(syngroupsfile)
doquery(db, q)
