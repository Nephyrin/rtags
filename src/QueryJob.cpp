/* This file is part of RTags.

RTags is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

RTags is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with RTags.  If not, see <http://www.gnu.org/licenses/>. */

#include "QueryJob.h"
#include "RTags.h"
#include <rct/EventLoop.h>
#include "Server.h"
#include "CursorInfo.h"
#include <regex>
#include "QueryMessage.h"
#include "Project.h"

// static int count = 0;
// static int active = 0;

QueryJob::QueryJob(const std::shared_ptr<QueryMessage> &query, unsigned int jobFlags, const std::shared_ptr<Project> &proj)
    : mAborted(false), mLinesWritten(0), mQueryMessage(query), mJobFlags(jobFlags), mProject(proj), mPathFilters(0),
      mPathFiltersRegex(0), mConnection(0)
{
    assert(query);
    if (query->flags() & QueryMessage::SilentQuery)
        setJobFlag(QuietJob);
    const List<String> &pathFilters = query->pathFilters();
    if (!pathFilters.isEmpty()) {
        if (query->flags() & QueryMessage::MatchRegex) {
            mPathFiltersRegex = new List<std::regex>();
            const int size = pathFilters.size();
            mPathFiltersRegex->reserve(size);
            for (int i=0; i<size; ++i) {
                mPathFiltersRegex->append(std::regex(pathFilters.at(i).ref()));
            }
        } else {
            mPathFilters = new List<String>(pathFilters);
        }
    }
}

QueryJob::QueryJob(unsigned int jobFlags, const std::shared_ptr<Project> &proj)
    : mAborted(false), mLinesWritten(0), mJobFlags(jobFlags), mProject(proj), mPathFilters(0),
      mPathFiltersRegex(0), mConnection(0)
{
}

QueryJob::~QueryJob()
{
    delete mPathFilters;
    delete mPathFiltersRegex;
}

uint32_t QueryJob::fileFilter() const
{
    if (mPathFilters && mPathFilters->size() == 1) {
        return Location::fileId(mPathFilters->first());
    }
    return 0;
}

bool QueryJob::write(const String &out, unsigned int flags)
{
    if ((mJobFlags & WriteUnfiltered) || (flags & Unfiltered) || filter(out)) {
        if ((mJobFlags & QuoteOutput) && !(flags & DontQuote)) {
            String o((out.size() * 2) + 2, '"');
            char *ch = o.data() + 1;
            int l = 2;
            for (int i=0; i<out.size(); ++i) {
                const char c = out.at(i);
                if (c == '"') {
                    *ch = '\\';
                    ch += 2;
                    l += 2;
                } else {
                    ++l;
                    *ch++ = c;
                }
            }
            o.truncate(l);
            return writeRaw(o, flags);
        } else {
            return writeRaw(out, flags);
        }
    }
    return true;
}

bool QueryJob::writeRaw(const String &out, unsigned int flags)
{
    assert(mConnection);
    if (!(flags & IgnoreMax) && mQueryMessage) {
        const int max = mQueryMessage->max();
        if (max != -1 && mLinesWritten == max) {
            return false;
        }
        assert(mLinesWritten < max || max == -1);
        ++mLinesWritten;
    }

    if (!(mJobFlags & QuietJob))
        error("=> %s", out.constData());

    if (mConnection) {
        if (!mConnection->write(out)) {
            abort();
            return false;
        }
        return true;
    }

    return true;
}

bool QueryJob::write(const Location &location, unsigned int /* flags */)
{
    if (location.isNull())
        return false;
    const int minLine = mQueryMessage ? mQueryMessage->minLine() : -1;
    if (minLine != -1) {
        assert(mQueryMessage);
        assert(mQueryMessage->maxLine() != -1);
        const int maxLine = mQueryMessage->maxLine();
        assert(maxLine != -1);
        const int line = location.line();
        if (line < minLine || line > maxLine) {
            return false;
        }
    }
    String out = location.key(keyFlags());
    const bool containingFunction = queryFlags() & QueryMessage::ContainingFunction;
    const bool cursorKind = queryFlags() & QueryMessage::CursorKind;
    const bool displayName = queryFlags() & QueryMessage::DisplayName;
    if (containingFunction || cursorKind || displayName) {
        const SymbolMap &symbols = project()->symbols();
        SymbolMap::const_iterator it = symbols.find(location);
        if (it == symbols.end()) {
            error() << "Somehow can't find" << location << "in symbols";
        } else {
            if (displayName)
                out += '\t' + it->second->displayName();
            if (cursorKind)
                out += '\t' + it->second->kindSpelling();
            if (containingFunction) {
                const uint32_t fileId = location.fileId();
                const unsigned int line = location.line();
                const unsigned int column = location.column();
                while (true) {
                    --it;
                    if (it->first.fileId() != fileId)
                        break;
                    if (it->second->isDefinition()
                        && RTags::isContainer(it->second->kind)
                        && comparePosition(line, column, it->second->startLine, it->second->startColumn) >= 0
                        && comparePosition(line, column, it->second->endLine, it->second->endColumn) <= 0) {
                        out += "\tfunction: " + it->second->symbolName;
                        break;
                    } else if (it == symbols.begin()) {
                        break;
                    }
                }
            }
        }
    }
    return write(out);
}

bool QueryJob::write(const std::shared_ptr<CursorInfo> &ci, unsigned int ciflags)
{
    if (!ci || ci->isNull())
        return false;
    const unsigned int kf = keyFlags();
    if (!write(ci->toString(ciflags, kf).constData()))
        return false;
    return true;
}

bool QueryJob::filter(const String &value) const
{
    if (!mPathFilters && !mPathFiltersRegex && !(queryFlags() & QueryMessage::FilterSystemIncludes))
        return true;

    const char *val = value.constData();
    while (*val && isspace(*val))
        ++val;

    if (queryFlags() & QueryMessage::FilterSystemIncludes && Path::isSystem(val))
        return false;

    if (!mPathFilters && !mPathFiltersRegex)
        return true;

    assert(!mPathFilters != !mPathFiltersRegex);
    String copy;
    const String &ref = (val != value.constData() ? copy : value);
    if (val != value.constData())
        copy = val;
    if (mPathFilters)
        return RTags::startsWith(*mPathFilters, ref);

    assert(mPathFiltersRegex);

    const int count = mPathFiltersRegex->size();
    for (int i=0; i<count; ++i) {
        if (std::regex_search(ref.constData(), mPathFiltersRegex->at(i)))
            return true;
    }
    return false;
}


int QueryJob::run(Connection *connection)
{
    assert(connection);
    mConnection = connection;
    const int ret = execute();
    mConnection = 0;
    return ret;
}
