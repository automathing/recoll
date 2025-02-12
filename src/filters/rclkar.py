#!/usr/bin/env python3

# Read a .kar midi karaoke file and translate to recoll indexable format

import rclexecm
import sys
import os.path
import string
import re
import codecs
from rclbasehandler import RclBaseHandler

try:
    import rcllatinclass
except:
    pass

try:
    import rclmidi as midi
except:
    print("RECFILTERROR HELPERNOTFOUND python3:midi")
    sys.exit(1)

try:
    import chardet

    has_chardet = True
except:
    has_chardet = False

# Prototype for the html document we're returning
htmltemplate = """
<html>
  <head>
    <meta http-equiv="content-type" content="text/html; charset=utf-8">
    <title>%s</title>
      <meta name="author" content="%s">
      <meta name="language" content="%s">
   </head>
   <body>
   %s
   </body>
</html>
"""

nlbytes = b"\n"
bsbytes = b"\\"
nullchar = 0


class KarTextExtractor(RclBaseHandler):
    # Afaik, the only charset encodings with null bytes are variations on
    # utf-16 and utf-32 and iso relatives. A hopefully comprehensive
    # list follows, compiled from iconv and python values. This is used for
    # stripping garbage from some files.
    acceptnullencodings = set(
        (
            "csucs4",
            "csunicode",
            "csunicode11",
            "iso-10646-ucs-2",
            "iso-10646-ucs-4",
            "u16",
            "u32",
            "ucs-2",
            "ucs-2-internal",
            "ucs-2-swapped",
            "ucs-2be",
            "ucs-2le",
            "ucs-4",
            "ucs-4-internal",
            "ucs-4-swapped",
            "ucs-4be",
            "ucs-4le",
            "unicode-1-1",
            "unicodebig",
            "unicodelittle",
            "utf-16",
            "utf-16be",
            "utf-16le",
            "utf-32",
            "utf-32be",
            "utf-32le",
            "utf16",
            "utf32",
            "utf_16",
            "utf_16_be",
            "utf_16_le",
            "utf_32",
            "utf_32_be",
            "utf_32_le",
        )
    )

    def __init__(self, em):
        super(KarTextExtractor, self).__init__(em)
        self.encoding = ""
        self.defaultencoding = ""
        self.hadnulls = False
        self.classifier = None

        # Compute the fallback encoding to use if we can't determine
        # one when processing the file. Based on the nls environment
        try:
            self.defaultencoding = sys.getfilesystemencoding()
        except:
            pass

        if self.defaultencoding is None:
            self.defaultencoding = sys.getdefaultencoding()

        if not self.defaultencoding or self.defaultencoding.lower().find("ascii") != -1:
            self.defaultencoding = "cp1252"

        try:
            codecs.lookup(self.defaultencoding)
        except:
            self.defaultencoding = "cp1252"

    def nulltrunc(self, data):
        """Truncate data after 1st null byte. For messages with garbage after
        a null byte. Must not be done for utf-16/32 of course"""

        if not data:
            return data
        try:
            firstnull = data.index(nullchar)
            self.hadnulls = True
            data = data[0:firstnull]
        except:
            pass
        return data

    def reencode(self, data):
        """Decode from whatever encoding we think this file is using
        and reencode as UTF-8"""

        # self.em.rclog("Reencoding from [%s] to UTF-8" % self.encoding)

        if data:
            try:
                data = data.decode(self.encoding, "ignore")
            except Exception as err:
                self.em.rclog("Decode failed: " + str(err))
                return ""
            try:
                data = data.encode("utf-8")
            except Exception as err:
                self.em.rclog("Encode failed: " + str(err))
                return ""

            data = rclexecm.htmlescape(data).decode("utf-8").replace("\n", "<br>\n")
        return data

    def encodingfromfilename(self, fn):
        """Compute encoding from file name: some karaoke files have the
        encoding as part of the file name as 'some title
        (encoding).xxx'. This is not an established convention though,
        just one our users could use if there is trouble with guessing
        encodings"""

        rexp = rb"\(([^\)]+)\)\.[a-zA-Z]+$"
        m = re.search(rexp, fn)
        if m:
            return m.group(1)
        else:
            return ""

    def chardet_detect(self, text):
        encodconf = chardet.detect(text)
        encoding = encodconf["encoding"]
        confidence = encodconf["confidence"]
        # self.em.rclog("Chardet returns %s %.2f" % (encoding,confidence))

        # chardet is awfully bad at detecting 8bit european
        # encodings/languages and will mostly return iso-8859-2 for
        # everything, which is a bad default (iso-8859-1/cp1252 being
        # much more common). We use our own ad-hoc stopwords based
        # module to try and improve
        if encoding.lower() == "iso-8859-2":
            if self.classifier is None:
                try:
                    import __main__

                    dir = os.path.dirname(__main__.__file__)
                    langszip = os.path.join(dir, "rcllatinstops.zip")
                    f = open(langszip)
                    f.close()
                    classifier = rcllatinclass.European8859TextClassifier(langszip)
                except:
                    self.em.rclog("Can't build euroclassifier (missing stopwords zip?")
                    return (encoding, confidence)

            try:
                lang, code, count = classifier.classify(text)
                # self.em.rclog("euclass lang/code/matchcount: %s %s %d" % \
                #              (lang, code, count))
                if count > 0:
                    confidence = 1.0
                    encoding = code
            except Exception as err:
                self.em.rclog("stopwords-based classifier failed: %s" % err)
                return (encoding, confidence)

        return (encoding, confidence)

    def html_text(self, filename):
        # Character encoding from file name ?
        self.encoding = self.encodingfromfilename(filename)
        if self.encoding:
            try:
                codecs.lookup(self.encoding)
            except:
                self.encoding = ""

        # Read in and midi-decode the file
        stream = midi.read_midifile(filename)

        title = None
        author = None
        language = None
        lyrics = b""
        lyricsN = b""
        self.hadnulls = False

        for event in stream.iterevents():
            edata = b""
            if isinstance(event, midi.TextMetaEvent):
                if not event.data:
                    continue
                elif event.data[0] == b"/"[0] or event.data[0] == bsbytes[0]:
                    edata = nlbytes + event.data[1:]
                elif event.data[0] == b"["[0] or event.data[0] == b"]"[0]:
                    edata = event.data[1:]
                elif event.data[0] == b"@"[0]:
                    if len(event.data) == 1:
                        continue
                    else:
                        if event.data[1] == b"I"[0]:
                            edata = event.data[2:] + nlbytes
                        elif event.data[1] == b"L"[0]:
                            language = self.nulltrunc(event.data[2:])
                            languageN = event.data[2:]
                        elif event.data[1] == b"T"[0]:
                            if title is None:
                                title = self.nulltrunc(event.data[2:])
                                titleN = event.data[2:]
                            elif author is None:
                                author = self.nulltrunc(event.data[2:])
                                authorN = event.data[2:]
                else:
                    edata = event.data
            elif isinstance(event, midi.LryricsEvent) or isinstance(
                event, midi.TrackNameEvent
            ):
                space = b""
                if isinstance(event, midi.TrackNameEvent):
                    nl = nlbytes
                if not event.data:
                    continue
                elif event.data[0] == b"/"[0] or event.data[0] == bsbytes[0]:
                    edata = nlbytes + event.data[1:] + nl
                else:
                    edata = event.data + nl

            lyrics += self.nulltrunc(edata)
            lyricsN += edata

        # Try to guess the encoding. First do it with the data
        # possibly containing nulls. If we get one of the accepted
        # nullbyte encodings, go with this, else repeat with the
        # de-nulled data

        # self.em.rclog("Lyrics length %d" % len(lyrics))

        if self.encoding == "" and has_chardet:
            if self.hadnulls:
                (encoding, confidence) = self.chardet_detect(lyricsN)
                # self.em.rclog("With nulls: chardet: enc [%s], conf %.2f" % \
                #              (encoding, confidence))
                if confidence > 0.6 and encoding.lower() in self.acceptnullencodings:
                    self.encoding = encoding
                    lyrics = lyricsN
                    title = titleN
                    author = authorN
            if self.encoding == "":
                (encoding, confidence) = self.chardet_detect(lyrics)
                # self.em.rclog("No nulls: chardet: enc [%s], conf %.2f" % \
                #              (encoding, confidence))
                if confidence > 0.6:
                    self.encoding = encoding

        if self.encoding == "":
            self.em.rclog(
                "Encoding not guessed, defaulting to [%s]" % (self.defaultencoding,)
            )
            self.encoding = self.defaultencoding

        if title is None:
            title = ""
        if author is None:
            author = ""
        if language is None:
            language = ""

        title = self.reencode(title)
        author = self.reencode(author)
        lyrics = self.reencode(lyrics)
        language = self.reencode(language)

        return htmltemplate % (title, author, language, lyrics)


proto = rclexecm.RclExecM()
extract = KarTextExtractor(proto)
rclexecm.main(proto, extract)
