/* Copyright (C) 2014-2019 J.F.Dockes
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include "autoconfig.h"

#include <stdio.h>

#include <string>

#include <QString>
#include <QStringList>
#include <QSettings>

#include "preview_plaintorich.h"
#include "preview_w.h"

#include "recoll.h"
#include "plaintorich.h"
#include "log.h"
#include "guiutils.h"
#include "cancelcheck.h"
#include "cstr.h"
#include "hldata.h"

using namespace std;

PlainToRichQtPreview::PlainToRichQtPreview()
{
    clear();
}

void PlainToRichQtPreview::clear()
{
    m_curanchor = 1; 
    m_lastanchor = 0;
    m_groupanchors.clear();
    m_groupcuranchors.clear();
    QSettings settings;
    m_spacehack = settings.value("anchorSpcHack", 0).toBool();
}

bool PlainToRichQtPreview::haveAnchors()
{
    return m_lastanchor != 0;
}

string  PlainToRichQtPreview::PlainToRichQtPreview::header()
{
    std::string fontstyle, hstyle;

#if defined(PREVIEW_WEBKIT) || defined(PREVIEW_WEBENGINE)
    hstyle = std::string{R"-(
<style>
.rclhighlight {
    color: red;
    background-color: yellow;
    font-size :150%;
}
</style>
)-"};
    fontstyle = prefs.htmlHeaderContents(true);
#endif
    
    if (m_inputhtml) {
        return fontstyle + hstyle;
    }

    std::string ret = std::string("<html><head><title></title>") + fontstyle + hstyle +
        "</head><body>";

    switch (prefs.previewPlainPre) {
    case PrefsPack::PP_BR:
        m_eolbr = true;
        break;
    case PrefsPack::PP_PRE:
        m_eolbr = false;
        ret += std::string("<pre>");
        break;
    case PrefsPack::PP_PREWRAP:
    default:
        m_eolbr = false;
        ret += std::string("<pre style=\"white-space: pre-wrap;");
        ret += "\">";
    }
    return ret;
}

string PlainToRichQtPreview::startMatch(unsigned int grpidx)
{
    LOGDEB2("startMatch, grpidx " << grpidx << "\n");
    grpidx = static_cast<unsigned int>(m_hdata->index_term_groups[grpidx].grpsugidx);
    LOGDEB2("startMatch, ugrpidx " << grpidx << "\n");
    m_groupanchors[grpidx].push_back(++m_lastanchor);
    m_groupcuranchors[grpidx] = 0;
    // We used to create the region as:
    //     <span style="..."><a name="...">term</a></span>
    // For some reason, when using a QTextBrowser, this caused problems with the display of some
    // Tamil text (qt bug?). Just inserting a space character after the opening <a tag, before the
    // text, clears the problem, reason unknown. We also inverted the <span and <a tags to avoid
    // highlighting the spurious space. The space hack only work in a <pre> section. Also: having <a
    // name=xxx></a> before the match term causes the same problem (so not a possible fix).  Space
    // does not seem to work any more (2021-04) ?
    //   Zero Width Non Joiner works but is displayed as ? sometimes on windows.
    //  nbsp seems to now work !
    string hackspace = m_spacehack? "&nbsp;" : "";
    string startmarker{
        "<a name='" + termAnchorName(m_lastanchor) + "'" +
        "id='" + termAnchorName(m_lastanchor) + "'" +  ">" +
        hackspace +
        "<span style='" + qs2utf8s(prefs.qtermstyle) + "'>" 
    };
    return startmarker;
}

string  PlainToRichQtPreview::endMatch()
{
    return "</span></a>";
}

string  PlainToRichQtPreview::termAnchorName(int i) const
{
    static const char *termAnchorNameBase = "TRM";
    char acname[sizeof(termAnchorNameBase) + 20];
    snprintf(acname, sizeof(acname), "%s%d", termAnchorNameBase, i);
    return string(acname);
}

string  PlainToRichQtPreview::startChunk()
{
    return "<pre>";
}

int  PlainToRichQtPreview::nextAnchorNum(int grpidx)
{
    LOGDEB2("nextAnchorNum: group " << grpidx << "\n");
    auto curit = m_groupcuranchors.find(grpidx);
    auto vecit = m_groupanchors.find(grpidx);
    if (grpidx == -1 || curit == m_groupcuranchors.end() ||
        vecit == m_groupanchors.end()) {
        if (m_curanchor >= m_lastanchor)
            m_curanchor = 1;
        else
            m_curanchor++;
    } else {
        if (curit->second >= vecit->second.size() -1)
            m_groupcuranchors[grpidx] = 0;
        else 
            m_groupcuranchors[grpidx]++;
        m_curanchor = vecit->second[m_groupcuranchors[grpidx]];
        LOGDEB2("nextAnchorNum: curanchor now " << m_curanchor << "\n");
    }
    return m_curanchor;
}

int  PlainToRichQtPreview::prevAnchorNum(int grpidx)
{
    auto curit = m_groupcuranchors.find(grpidx);
    auto vecit = m_groupanchors.find(grpidx);
    if (grpidx == -1 || curit == m_groupcuranchors.end() ||
        vecit == m_groupanchors.end()) {
        if (m_curanchor <= 1)
            m_curanchor = m_lastanchor;
        else
            m_curanchor--;
    } else {
        if (curit->second <= 0)
            m_groupcuranchors[grpidx] = static_cast<unsigned int>(vecit->second.size() -1);
        else 
            m_groupcuranchors[grpidx]--;
        m_curanchor = vecit->second[m_groupcuranchors[grpidx]];
    }
    return m_curanchor;
}

QString  PlainToRichQtPreview::curAnchorName() const
{
    return u8s2qs(termAnchorName(m_curanchor));
}


ToRichThread::ToRichThread(const string &i, const HighlightData& hd,
                           std::shared_ptr<PlainToRichQtPreview> ptr,
                           QStringList& qrichlist,
                           QObject *parent)
    : QThread(parent), m_input(i), m_hdata(hd), m_ptr(ptr), m_output(qrichlist)
{
}

// Insert into editor by chunks so that the top becomes visible
// earlier for big texts. This provokes some artifacts (adds empty line),
// so we can't set it too low.
#define CHUNKL 500*1000

void ToRichThread::run()
{
    list<string> out;
    try {
        m_ptr->plaintorich(m_input, out, m_hdata, CHUNKL);
    } catch (CancelExcept) {
        return;
    }

    // Convert C++ string list to QString list
    for (const auto& chunk : out) {
        m_output.push_back(u8s2qs(chunk));
    }
}
