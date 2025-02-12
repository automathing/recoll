#!/usr/bin/env python3
"""
Modified from github:honza/mutt-notmuch-py

This is a recoll version of the original mutt-notmuch script.

It will interactively ask you for a search query and then symlink the matching
messages to $HOME/.cache/mutt_results.

Add this to your .muttrc.

macro index / "<enter-command>unset wait_key<enter><shell-escape>mutt-recoll.py<enter><change-folder-readonly>~/.cache/mutt_results<enter>" \
          "search mail (using recoll)"

This script overrides the $HOME/.cache/mutt_results each time you run a query.

Install this by adding this file somewhere on your PATH.

(c) 2012 - Honza Pokorny
(c) 2014 - Jean-Francois Dockes
Licensed under BSD
"""

import os
import hashlib

from commands import getoutput
from mailbox import Maildir
from optparse import OptionParser
from collections import defaultdict


def digest(filename):
    with open(filename) as f:
        return hashlib.sha1(f.read()).hexdigest()


def pick_all_mail(messages):
    for m in messages:
        if "All Mail" in m:
            return m


def empty_dir(directory):
    box = Maildir(directory)
    box.clear()


def command(cmd):
    return getoutput(cmd)


def main(dest_box, is_gmail):
    query = raw_input("Query: ")

    command("mkdir -p %s/cur" % dest_box)
    command("mkdir -p %s/new" % dest_box)

    empty_dir(dest_box)

    files = command("recoll -t -b -q %s" % query).split("\n")

    data = defaultdict(list)
    messages = []

    for f in files:
        # Recoll outputs file:// urls
        f = f[7:]
        if not f:
            continue

        try:
            sha = digest(f)
            data[sha].append(f)
        except IOError:
            print("File %s does not exist" % f)

    for sha in data:
        if is_gmail and len(data[sha]) > 1:
            messages.append(pick_all_mail(data[sha]))
        else:
            messages.append(data[sha][0])

    for m in messages:
        if not m:
            continue

        target = os.path.join(dest_box, "cur", os.path.basename(m))
        if not os.path.exists(target):
            print("symlink [%s] -> [%s]" % (m, target))
            os.symlink(m, target)


if __name__ == "__main__":
    p = OptionParser("usage: %prog [OPTIONS] [RESULTDIR]")
    p.add_option(
        "-g",
        "--gmail",
        dest="gmail",
        action="store_true",
        default=True,
        help="gmail-specific behavior",
    )
    p.add_option(
        "-G",
        "--not-gmail",
        dest="gmail",
        action="store_false",
        help="gmail-specific behavior",
    )
    (options, args) = p.parse_args()

    if args:
        dest = args[0]
    else:
        dest = os.path.expanduser("~/.cache/mutt_results")
        if not os.path.exists(dest):
            os.makedirs(dest)

    # Use expanduser() so that os.symlink() won't get weirded out by tildes.
    main(os.path.expanduser(dest).rstrip("/"), options.gmail)
