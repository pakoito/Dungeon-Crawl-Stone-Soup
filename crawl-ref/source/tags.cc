/**
 * @file
 * @brief Auxiliary functions to make savefile versioning simpler.
**/

/*
   The marshalling and unmarshalling of data is done in big endian and
   is meant to keep savefiles cross-platform.  Note also that the marshalling
   sizes are 1, 2, and 4 for byte, short, and int.  If a strange platform
   with different sizes of these basic types pops up, please sed it to fixed-
   width ones.  For now, that wasn't done in order to keep things convenient.
*/

#include "AppHdr.h"
#include "bitary.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>            // for memcpy
#include <iterator>
#include <algorithm>
#include <vector>

#ifdef UNIX
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "abyss.h"
#include "art-enum.h"
#include "artefact.h"
#include "branch.h"
#include "butcher.h"
#include "colour.h"
#include "coord.h"
#include "coordit.h"
#include "describe.h"
#include "dbg-scan.h"
#include "dgn-overview.h"
#include "dungeon.h"
#include "end.h"
#include "enum.h"
#include "errors.h"
#include "map_knowledge.h"
#include "externs.h"
#include "files.h"
#include "ghost.h"
#include "godcompanions.h"
#include "itemname.h"
#include "itemprop.h"
#include "items.h"
#include "libutil.h"
#include "mapmark.h"
#include "misc.h"
#include "mon-death.h"
#include "mon-info.h"
#if TAG_MAJOR_VERSION == 34
 #include "mon-chimera.h"
#endif
#include "mon-util.h"
#include "mon-transit.h"
#include "place.h"
#include "quiver.h"
#include "religion.h"
#include "skills.h"
#include "skills2.h"
#include "state.h"
#include "env.h"
#include "spl-wpnench.h"
#include "stringutil.h"
#include "syscalls.h"
#include "tags.h"
#include "terrain.h"
#include "tiledef-dngn.h"
#include "tiledef-player.h"
#include "tilepick.h"
#include "tileview.h"
#ifdef USE_TILE
 #include "tilemcache.h"
#endif
#include "transform.h"
#include "travel.h"
#include "unwind.h"
#include "version.h"

// defined in dgn-overview.cc
extern map<branch_type, set<level_id> > stair_level;
extern map<level_pos, shop_type> shops_present;
extern map<level_pos, god_type> altars_present;
extern map<level_pos, branch_type> portals_present;
extern map<level_pos, string> portal_notes;
extern map<level_id, string> level_annotations;
extern map<level_id, string> level_exclusions;
extern map<level_id, string> level_uniques;
extern set<pair<string, level_id> > auto_unique_annotations;

// defined in abyss.cc
extern abyss_state abyssal_state;

reader::reader(const string &_read_filename, int minorVersion)
    : _filename(_read_filename), _chunk(0), _pbuf(NULL), _read_offset(0),
      _minorVersion(minorVersion), _safe_read(false)
{
    _file       = fopen_u(_filename.c_str(), "rb");
    opened_file = !!_file;
}

reader::reader(package *save, const string &chunkname, int minorVersion)
    : _file(0), _chunk(0), opened_file(false), _pbuf(0), _read_offset(0),
     _minorVersion(minorVersion), _safe_read(false)
{
    ASSERT(save);
    _chunk = new chunk_reader(save, chunkname);
}

reader::~reader()
{
    if (_chunk)
        delete _chunk;
    close();
}

void reader::close()
{
    if (opened_file && _file)
        fclose(_file);
    _file = NULL;
}

void reader::advance(size_t offset)
{
    char junk[128];

    while (offset)
    {
        const size_t junklen = min(sizeof(junk), offset);
        offset -= junklen;
        read(junk, junklen);
    }
}

bool reader::valid() const
{
    return (_file && !feof(_file)) ||
           (_pbuf && _read_offset < _pbuf->size());
}

static NORETURN void _short_read(bool safe_read)
{
    if (!crawl_state.need_save || safe_read)
        throw short_read_exception();
    // Would be nice to name the save chunk here, but in interesting cases
    // we're reading a copy from memory (why?).
    die_noline("short read while reading save");
}

// Reads input in network byte order, from a file or buffer.
unsigned char reader::readByte()
{
    if (_file)
    {
        int b = fgetc(_file);
        if (b == EOF)
            _short_read(_safe_read);
        return b;
    }
    else if (_chunk)
    {
        unsigned char buf;
        if (_chunk->read(&buf, 1) != 1)
            _short_read(_safe_read);
        return buf;
    }
    else
    {
        if (_read_offset >= _pbuf->size())
            _short_read(_safe_read);
        return (*_pbuf)[_read_offset++];
    }
}

void reader::read(void *data, size_t size)
{
    if (_file)
    {
        if (data)
        {
            if (fread(data, 1, size, _file) != size)
                _short_read(_safe_read);
        }
        else
            fseek(_file, (long)size, SEEK_CUR);
    }
    else if (_chunk)
    {
        if (_chunk->read(data, size) != size)
            _short_read(_safe_read);
    }
    else
    {
        if (_read_offset+size > _pbuf->size())
            _short_read(_safe_read);
        if (data && size)
            memcpy(data, &(*_pbuf)[_read_offset], size);

        _read_offset += size;
    }
}

int reader::getMinorVersion() const
{
    ASSERT(_minorVersion != TAG_MINOR_INVALID);
    return _minorVersion;
}

void reader::setMinorVersion(int minorVersion)
{
    _minorVersion = minorVersion;
}

void reader::fail_if_not_eof(const string &name)
{
    char dummy;
    if (_chunk ? _chunk->read(&dummy, 1) :
        _file ? (fgetc(_file) != EOF) :
        _read_offset >= _pbuf->size())
    {
        fail("Incomplete read of \"%s\" - aborting.", name.c_str());
    }
}

void writer::check_ok(bool ok)
{
    if (!ok && !failed)
    {
        failed = true;
        if (!_ignore_errors)
            end(1, true, "Error writing to %s", _filename.c_str());
    }
}

void writer::writeByte(unsigned char ch)
{
    if (failed)
        return;

    if (_chunk)
        _chunk->write(&ch, 1);
    else if (_file)
        check_ok(fputc(ch, _file) != EOF);
    else
        _pbuf->push_back(ch);
}

void writer::write(const void *data, size_t size)
{
    if (failed)
        return;

    if (_chunk)
        _chunk->write(data, size);
    else if (_file)
        check_ok(fwrite(data, 1, size, _file) == size);
    else
    {
        const unsigned char* cdata = static_cast<const unsigned char*>(data);
        _pbuf->insert(_pbuf->end(), cdata, cdata+size);
    }
}

long writer::tell()
{
    ASSERT(!_chunk);
    return _file? ftell(_file) : _pbuf->size();
}

#ifdef DEBUG_GLOBALS
// Force a conditional jump valgrind may pick up, no matter the optimizations.
static volatile uint32_t hashroll;
static void CHECK_INITIALIZED(uint32_t x)
{
    hashroll = 0;
    if ((hashroll += x) & 1)
        hashroll += 2;
}
#else
#define CHECK_INITIALIZED(x)
#endif

// static helpers
static void tag_construct_char(writer &th);
static void tag_construct_you(writer &th);
static void tag_construct_you_items(writer &th);
static void tag_construct_you_dungeon(writer &th);
static void tag_construct_lost_monsters(writer &th);
static void tag_construct_lost_items(writer &th);
static void tag_construct_companions(writer &th);
static void tag_read_you(reader &th);
static void tag_read_you_items(reader &th);
static void tag_read_you_dungeon(reader &th);
static void tag_read_lost_monsters(reader &th);
static void tag_read_lost_items(reader &th);
static void tag_read_companions(reader &th);

static void tag_construct_level(writer &th);
static void tag_construct_level_items(writer &th);
static void tag_construct_level_monsters(writer &th);
static void tag_construct_level_tiles(writer &th);
static void tag_read_level(reader &th);
static void tag_read_level_items(reader &th);
static void tag_read_level_monsters(reader &th);
static void tag_read_level_tiles(reader &th);
static void _regenerate_tile_flavour();
static void _draw_tiles();

static void tag_construct_ghost(writer &th);
static void tag_read_ghost(reader &th);

static void marshallGhost(writer &th, const ghost_demon &ghost);
static ghost_demon unmarshallGhost(reader &th);

static void marshallSpells(writer &, const monster_spells &);
static void unmarshallSpells(reader &, monster_spells &);

static void marshallMonsterInfo (writer &, const monster_info &);
static void unmarshallMonsterInfo (reader &, monster_info &mi);
static void marshallMapCell (writer &, const map_cell &);
static void unmarshallMapCell (reader &, map_cell& cell);

template<typename T, typename T_iter, typename T_marshal>
static void marshall_iterator(writer &th, T_iter beg, T_iter end,
                              T_marshal marshal);
template<typename T>
static void unmarshall_vector(reader& th, vector<T>& vec,
                              T (*T_unmarshall)(reader&));

template<int SIZE>
void marshallFixedBitVector(writer& th, const FixedBitVector<SIZE>& arr);
template<int SIZE>
void unmarshallFixedBitVector(reader& th, FixedBitVector<SIZE>& arr);

void marshallByte(writer &th, int8_t data)
{
    CHECK_INITIALIZED(data);
    th.writeByte(data);
}

int8_t unmarshallByte(reader &th)
{
    return th.readByte();
}

void marshallUByte(writer &th, uint8_t data)
{
    CHECK_INITIALIZED(data);
    th.writeByte(data);
}

uint8_t unmarshallUByte(reader &th)
{
    return th.readByte();
}

// Marshall 2 byte short in network order.
void marshallShort(writer &th, short data)
{
    CHECK_INITIALIZED(data);
    const char b2 = (char)(data & 0x00FF);
    const char b1 = (char)((data & 0xFF00) >> 8);
    th.writeByte(b1);
    th.writeByte(b2);
}

// Unmarshall 2 byte short in network order.
int16_t unmarshallShort(reader &th)
{
    int16_t b1 = th.readByte();
    int16_t b2 = th.readByte();
    int16_t data = (b1 << 8) | (b2 & 0x00FF);
    return data;
}

// Marshall 4 byte int in network order.
void marshallInt(writer &th, int32_t data)
{
    CHECK_INITIALIZED(data);
    char b4 = (char) (data & 0x000000FF);
    char b3 = (char)((data & 0x0000FF00) >> 8);
    char b2 = (char)((data & 0x00FF0000) >> 16);
    char b1 = (char)((data & 0xFF000000) >> 24);

    th.writeByte(b1);
    th.writeByte(b2);
    th.writeByte(b3);
    th.writeByte(b4);
}

// Unmarshall 4 byte signed int in network order.
int32_t unmarshallInt(reader &th)
{
    int32_t b1 = th.readByte();
    int32_t b2 = th.readByte();
    int32_t b3 = th.readByte();
    int32_t b4 = th.readByte();

    int32_t data = (b1 << 24) | ((b2 & 0x000000FF) << 16);
    data |= ((b3 & 0x000000FF) << 8) | (b4 & 0x000000FF);
    return data;
}

void marshallUnsigned(writer& th, uint64_t v)
{
    do
    {
        unsigned char b = (unsigned char)(v & 0x7f);
        v >>= 7;
        if (v)
            b |= 0x80;
        th.writeByte(b);
    }
    while (v);
}

uint64_t unmarshallUnsigned(reader& th)
{
    unsigned i = 0;
    uint64_t v = 0;
    for (;;)
    {
        unsigned char b = th.readByte();
        v |= (uint64_t)(b & 0x7f) << i;
        i += 7;
        if (!(b & 0x80))
            break;
    }
    return v;
}

void marshallSigned(writer& th, int64_t v)
{
    if (v < 0)
        marshallUnsigned(th, (uint64_t)((-v - 1) << 1) | 1);
    else
        marshallUnsigned(th, (uint64_t)(v << 1));
}

int64_t unmarshallSigned(reader& th)
{
    uint64_t u;
    unmarshallUnsigned(th, u);
    if (u & 1)
        return (int64_t)(-(u >> 1) - 1);
    else
        return (int64_t)(u >> 1);
}

// Optimized for short vectors that have only the first few bits set, and
// can have invalid length.  For long ones you might want to do this
// differently to not lose 1/8 bits and speed.
template<int SIZE>
void marshallFixedBitVector(writer& th, const FixedBitVector<SIZE>& arr)
{
    int last_bit;
    for (last_bit = SIZE - 1; last_bit > 0; last_bit--)
        if (arr[last_bit])
            break;

    int i = 0;
    while (1)
    {
        uint8_t byte = 0;
        for (int j = 0; j < 7; j++)
            if (i < SIZE && arr[i++])
                byte |= 1 << j;
        if (i <= last_bit)
            marshallUByte(th, byte);
        else
        {
            marshallUByte(th, byte | 0x80);
            break;
        }
    }
}

template<int SIZE>
void unmarshallFixedBitVector(reader& th, FixedBitVector<SIZE>& arr)
{
    arr.reset();

    int i = 0;
    while (1)
    {
        uint8_t byte = unmarshallUByte(th);
        for (int j = 0; j < 7; j++)
            if (i < SIZE)
                arr.set(i++, !!(byte & (1 << j)));
        if (byte & 0x80)
            break;
    }
}

// FIXME: Kill this abomination - it will break!
template<typename T>
static void _marshall_as_int(writer& th, const T& t)
{
    marshallInt(th, static_cast<int>(t));
}

template <typename data>
void marshallSet(writer &th, const set<data> &s,
                 void (*marshall)(writer &, const data &))
{
    marshallInt(th, s.size());
    typename set<data>::const_iterator i = s.begin();
    for (; i != s.end(); ++i)
        marshall(th, *i);
}

template<typename key, typename value>
void marshallMap(writer &th, const map<key,value>& data,
                 void (*key_marshall)(writer&, const key&),
                 void (*value_marshall)(writer&, const value&))
{
    marshallInt(th, data.size());
    typename map<key,value>::const_iterator ci;
    for (ci = data.begin(); ci != data.end(); ++ci)
    {
        key_marshall(th, ci->first);
        value_marshall(th, ci->second);
    }
}

template<typename T_iter, typename T_marshall_t>
static void marshall_iterator(writer &th, T_iter beg, T_iter end,
                              T_marshall_t T_marshall)
{
    marshallInt(th, distance(beg, end));
    while (beg != end)
    {
        T_marshall(th, *beg);
        ++beg;
    }
}

template<typename T>
static void unmarshall_vector(reader& th, vector<T>& vec,
                              T (*T_unmarshall)(reader&))
{
    vec.clear();
    const int num_to_read = unmarshallInt(th);
    for (int i = 0; i < num_to_read; ++i)
        vec.push_back(T_unmarshall(th));
}

template <typename T_container, typename T_inserter, typename T_unmarshall>
static void unmarshall_container(reader &th, T_container &container,
                                 T_inserter inserter, T_unmarshall unmarshal)
{
    container.clear();
    const int num_to_read = unmarshallInt(th);
    for (int i = 0; i < num_to_read; ++i)
        (container.*inserter)(unmarshal(th));
}

void marshall_level_id(writer& th, const level_id& id)
{
    marshallShort(th, id.packed_place());
}

static void _marshall_level_id_set(writer& th, const set<level_id>& id)
{
    marshallSet(th, id, marshall_level_id);
}

// XXX: Redundant with level_pos.save()/load().
static void _marshall_level_pos(writer& th, const level_pos& lpos)
{
    marshallInt(th, lpos.pos.x);
    marshallInt(th, lpos.pos.y);
    marshall_level_id(th, lpos.id);
}

template <typename data, typename set>
void unmarshallSet(reader &th, set &dset,
                   data (*data_unmarshall)(reader &))
{
    dset.clear();
    int len = unmarshallInt(th);
    for (int i = 0; i < len; ++i)
        dset.insert(data_unmarshall(th));
}

template<typename key, typename value, typename map>
void unmarshallMap(reader& th, map& data,
                   key   (*key_unmarshall)  (reader&),
                   value (*value_unmarshall)(reader&))
{
    int i, len = unmarshallInt(th);
    key k;
    for (i = 0; i < len; ++i)
    {
        k = key_unmarshall(th);
        pair<key, value> p(k, value_unmarshall(th));
        data.insert(p);
    }
}

template<typename T>
static T unmarshall_int_as(reader& th)
{
    return static_cast<T>(unmarshallInt(th));
}

level_id unmarshall_level_id(reader& th)
{
    return level_id::from_packed_place(unmarshallShort(th));
}

static set<level_id> _unmarshall_level_id_set(reader& th)
{
    set<level_id> id;
    unmarshallSet(th, id, unmarshall_level_id);
    return id;
}

static level_pos _unmarshall_level_pos(reader& th)
{
    level_pos lpos;
    lpos.pos.x = unmarshallInt(th);
    lpos.pos.y = unmarshallInt(th);
    lpos.id    = unmarshall_level_id(th);
    return lpos;
}

void marshallCoord(writer &th, const coord_def &c)
{
    marshallInt(th, c.x);
    marshallInt(th, c.y);
}

coord_def unmarshallCoord(reader &th)
{
    coord_def c;
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() >= TAG_MINOR_COORD_SERIALIZER
        && th.getMinorVersion() != TAG_MINOR_0_11)
    {
#endif
        c.x = unmarshallInt(th);
        c.y = unmarshallInt(th);
#if TAG_MAJOR_VERSION == 34
    }
    else
    {
        c.x = unmarshallShort(th);
        c.y = unmarshallShort(th);
    }
#endif
    return c;
}

#if TAG_MAJOR_VERSION == 34
// Between TAG_MINOR_OPTIONAL_PARTS and TAG_MINOR_FIXED_CONSTRICTION
// we neglected to marshall the constricting[] map of monsters.  Fix
// those up.
static void _fix_missing_constrictions()
{
    for (int i = -1; i < MAX_MONSTERS; ++i)
    {
        const actor* m = i < 0 ? (actor*)&you : (actor*)&menv[i];
        if (!m->alive())
            continue;
        if (!m->constricted_by)
            continue;
        actor *h = actor_by_mid(m->constricted_by);
        // Not a known bug, so don't fix this up.
        if (!h)
            continue;

        if (!h->constricting)
            h->constricting = new actor::constricting_t;
        if (h->constricting->find(m->mid) == h->constricting->end())
        {
            dprf("Fixing missing constriction for %s (mindex=%d mid=%d)"
                 " of %s (mindex=%d mid=%d)",
                 h->name(DESC_PLAIN, true).c_str(), h->mindex(), h->mid,
                 m->name(DESC_PLAIN, true).c_str(), m->mindex(), m->mid);

            (*h->constricting)[m->mid] = 0;
        }
    }
}
#endif

static void _marshall_constriction(writer &th, const actor *who)
{
    _marshall_as_int(th, who->held);
    marshallInt(th, who->constricted_by);
    marshallInt(th, who->escape_attempts);

    // Assumes an empty map is marshalled as just the int 0.
    const actor::constricting_t * const cmap = who->constricting;
    if (cmap)
        marshallMap(th, *cmap, _marshall_as_int<mid_t>, _marshall_as_int<int>);
    else
        marshallInt(th, 0);
}

static void _unmarshall_constriction(reader &th, actor *who)
{
    who->held = unmarshall_int_as<held_type>(th);
    who->constricted_by = unmarshallInt(th);
    who->escape_attempts = unmarshallInt(th);

    actor::constricting_t cmap;
    unmarshallMap(th, cmap, unmarshall_int_as<mid_t>, unmarshallInt);

    if (cmap.size() == 0)
        who->constricting = 0;
    else
        who->constricting = new actor::constricting_t(cmap);
}

template <typename marshall, typename grid>
static void _run_length_encode(writer &th, marshall m, const grid &g,
                               int width, int height)
{
    int last = 0, nlast = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
        {
            if (!nlast)
                last = g[x][y];
            if (last == g[x][y] && nlast < 255)
            {
                nlast++;
                continue;
            }

            marshallByte(th, nlast);
            m(th, last);

            last = g[x][y];
            nlast = 1;
        }

    marshallByte(th, nlast);
    m(th, last);
}

template <typename unmarshall, typename grid>
static void _run_length_decode(reader &th, unmarshall um, grid &g,
                               int width, int height)
{
    const int end = width * height;
    int offset = 0;
    while (offset < end)
    {
        const int run = unmarshallUByte(th);
        const int value = um(th);

        for (int i = 0; i < run; ++i)
        {
            const int y = offset / width;
            const int x = offset % width;
            g[x][y] = value;
            ++offset;
        }
    }
}

union float_marshall_kludge
{
    // [ds] Does ANSI C guarantee that sizeof(float) == sizeof(long)?
    // [haranp] no, unfortunately
    // [1KB] on 64 bit arches, long is 64 bits, while float is 32 bits.
    float    f_num;
    int32_t  l_num;
};

// single precision float -- marshall in network order.
void marshallFloat(writer &th, float data)
{
    float_marshall_kludge k;
    k.f_num = data;
    marshallInt(th, k.l_num);
}

// single precision float -- unmarshall in network order.
float unmarshallFloat(reader &th)
{
    float_marshall_kludge k;
    k.l_num = unmarshallInt(th);
    return k.f_num;
}

// string -- 2 byte length, string data
void marshallString(writer &th, const string &data)
{
    size_t len = data.length();
    // A limit of 32K.
    if (len > SHRT_MAX)
        die("trying to marshall too long a string (len=%ld)", (long int)len);
    marshallShort(th, len);

    th.write(data.c_str(), len);
}

string unmarshallString(reader &th)
{
    char buffer[SHRT_MAX];

    short len = unmarshallShort(th);
    ASSERT(len >= 0);
    ASSERT(len <= (ssize_t)sizeof(buffer));

    th.read(buffer, len);

    return string(buffer, len);
}

// This one must stay with a 16 bit signed big-endian length tag, to allow
// older versions to browse and list newer saves.
static void marshallString2(writer &th, const string &data)
{
    marshallString(th, data);
}

static string unmarshallString2(reader &th)
{
    return unmarshallString(th);
}

// string -- 4 byte length, non-terminated string data.
void marshallString4(writer &th, const string &data)
{
    marshallInt(th, data.length());
    th.write(data.c_str(), data.length());
}
void unmarshallString4(reader &th, string& s)
{
    const int len = unmarshallInt(th);
    s.resize(len);
    if (len) th.read(&s.at(0), len);
}

// boolean (to avoid system-dependent bool implementations)
void marshallBoolean(writer &th, bool data)
{
    th.writeByte(data ? 1 : 0);
}

// boolean (to avoid system-dependent bool implementations)
bool unmarshallBoolean(reader &th)
{
    return th.readByte() != 0;
}

// Saving the date as a string so we're not reliant on a particular epoch.
string make_date_string(time_t in_date)
{
    if (in_date <= 0)
        return "";

    struct tm *date = TIME_FN(&in_date);

    return make_stringf(
              "%4d%02d%02d%02d%02d%02d%s",
              date->tm_year + 1900, date->tm_mon, date->tm_mday,
              date->tm_hour, date->tm_min, date->tm_sec,
              ((date->tm_isdst > 0) ? "D" : "S"));
}

static void marshallStringVector(writer &th, const vector<string> &vec)
{
    marshall_iterator(th, vec.begin(), vec.end(), marshallString);
}

static vector<string> unmarshallStringVector(reader &th)
{
    vector<string> vec;
    unmarshall_vector(th, vec, unmarshallString);
    return vec;
}

static monster_type unmarshallMonType(reader &th)
{
    monster_type x = static_cast<monster_type>(unmarshallShort(th));

    if (x >= MONS_NO_MONSTER)
        return x;

#if TAG_MAJOR_VERSION == 34
# define AXED(a) if (x > a) --x
    if (th.getMinorVersion() == TAG_MINOR_0_11)
    {
        AXED(MONS_KILLER_BEE); // killer bee larva
        AXED(MONS_SHADOW_IMP); // midge
        AXED(MONS_AGNES);      // Jozef
    }
#endif

    return x;
}

#if TAG_MAJOR_VERSION == 34
// yay marshalling inconsistencies
static monster_type unmarshallMonType_Info(reader &th)
{
    monster_type x = static_cast<monster_type>(unmarshallUnsigned(th));

    if (x >= MONS_NO_MONSTER)
        return x;

    if (th.getMinorVersion() == TAG_MINOR_0_11)
    {
        AXED(MONS_KILLER_BEE); // killer bee larva
        AXED(MONS_SHADOW_IMP); // midge
        AXED(MONS_AGNES);      // Jozef
    }

    return x;
}
#endif

static spell_type unmarshallSpellType(reader &th
#if TAG_MAJOR_VERSION == 34
                                      , bool mons = false
#endif
                                      )
{
    spell_type x = SPELL_NO_SPELL;
#if TAG_MAJOR_VERSION == 34
    if (!mons && th.getMinorVersion() < TAG_MINOR_SHORT_SPELL_TYPE)
        x = static_cast<spell_type>(unmarshallUByte(th));
    else
#endif
        x = static_cast<spell_type>(unmarshallShort(th));

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() == TAG_MINOR_0_11)
    {
        AXED(SPELL_DEBUGGING_RAY); // projected noise
        AXED(SPELL_HEAL_OTHER);    // summon greater holy
    }
#endif

    return x;
}

static dungeon_feature_type unmarshallFeatureType(reader &th)
{
    dungeon_feature_type x = static_cast<dungeon_feature_type>(unmarshallUByte(th));

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() == TAG_MINOR_0_11)
    {
        // turn old trees into...trees
        if (x == DNGN_OPEN_SEA)
            x = DNGN_TREE;
        else if (x >= DNGN_LAVA_SEA && x < 30)
            x = (dungeon_feature_type)(x - 1);
    }
    // turn mangroves into trees
    else if (th.getMinorVersion() < TAG_MINOR_MANGROVES && x == DNGN_OPEN_SEA)
        x = DNGN_TREE;
    else if (th.getMinorVersion() < TAG_MINOR_FIX_FEAT_SHIFT
             && x > DNGN_OPEN_SEA && x < DNGN_LAVA)
    {
        x = static_cast<dungeon_feature_type>(x - 1);
    }

    if (x >= DNGN_DRY_FOUNTAIN_BLUE && x <= DNGN_DRY_FOUNTAIN_BLOOD)
        x = DNGN_DRY_FOUNTAIN;
#endif

    return x;
}

#if TAG_MAJOR_VERSION == 34
// yay marshalling inconsistencies
static dungeon_feature_type unmarshallFeatureType_Info(reader &th)
{
    dungeon_feature_type x = static_cast<dungeon_feature_type>(unmarshallUnsigned(th));

    if (th.getMinorVersion() == TAG_MINOR_0_11)
    {
        if (x == DNGN_OPEN_SEA)
            x = DNGN_TREE;
        else if (x >= DNGN_LAVA_SEA && x < 30)
            x = (dungeon_feature_type)(x - 1);
    }
    else if (th.getMinorVersion() < TAG_MINOR_MANGROVES && x == DNGN_OPEN_SEA)
        x = DNGN_TREE;

    return x;
}
#endif

#define CANARY     marshallUByte(th, 171)
#if TAG_MAJOR_VERSION == 34
#define EAT_CANARY do if (th.getMinorVersion() >= TAG_MINOR_CANARIES    \
                          && unmarshallUByte(th) != 171)                \
                   {                                                    \
                       die("save corrupted: canary gone");              \
                   } while (0)
#else
#define EAT_CANARY do if ( unmarshallUByte(th) != 171)                  \
                   {                                                    \
                       die("save corrupted: canary gone");              \
                   } while (0)
#endif

#if TAG_MAJOR_VERSION == 34
static void _ensure_entry(branch_type br)
{
    dungeon_feature_type entry = branches[br].entry_stairs;
    for (rectangle_iterator ri(1); ri; ++ri)
        if (grd(*ri) == entry)
            return;

    // Find primary upstairs.
    for (rectangle_iterator ri(1); ri; ++ri)
        if (grd(*ri) == DNGN_STONE_STAIRS_UP_I)
        {
            for (distance_iterator di(*ri); di; ++di)
                if (in_bounds(*di) && grd(*di) == DNGN_FLOOR)
                {
                    grd(*di) = entry; // No need to update LOS, etc.
                    // Announce the repair even in non-debug builds.
                    mprf(MSGCH_ERROR, "Placing missing branch entry: %s.",
                         dungeon_feature_name(entry));
                    return;
                }
            die("no floor to place a branch entrance");
        }
    die("no upstairs on %s???", level_id::current().describe().c_str());
}

static void _add_missing_branches()
{
    const level_id lc = level_id::current();

    // Could do all just in case, but this seems safer:
    if (brentry[BRANCH_VAULTS] == lc)
        _ensure_entry(BRANCH_VAULTS);
    if (brentry[BRANCH_ZOT] == lc)
        _ensure_entry(BRANCH_ZOT);
    if (lc == level_id(BRANCH_DEPTHS, 2) || lc == level_id(BRANCH_DUNGEON, 21))
        _ensure_entry(BRANCH_VESTIBULE);
    if (lc == level_id(BRANCH_DEPTHS, 3) || lc == level_id(BRANCH_DUNGEON, 24))
        _ensure_entry(BRANCH_PANDEMONIUM);
    if (lc == level_id(BRANCH_DEPTHS, 4) || lc == level_id(BRANCH_DUNGEON, 25))
        _ensure_entry(BRANCH_ABYSS);
    if (player_in_branch(BRANCH_VESTIBULE))
    {
        for (rectangle_iterator ri(0); ri; ++ri)
        {
            if (grd(*ri) == DNGN_STONE_ARCH)
            {
                map_marker *marker = env.markers.find(*ri, MAT_FEATURE);
                if (marker)
                {
                    map_feature_marker *featm =
                        dynamic_cast<map_feature_marker*>(marker);
                    // [ds] Ensure we're activating the correct feature
                    // markers. Feature markers are also used for other
                    // things, notably to indicate the return point from
                    // a labyrinth or portal vault.
                    switch (featm->feat)
                    {
                    case DNGN_ENTER_COCYTUS:
                    case DNGN_ENTER_DIS:
                    case DNGN_ENTER_GEHENNA:
                    case DNGN_ENTER_TARTARUS:
                        grd(*ri) = featm->feat;
                        dprf("opened %s", dungeon_feature_name(featm->feat));
                        env.markers.remove(marker);
                        break;
                    default:
                        break;
                    }
                }
            }
        }
    }
}
#endif


// Write a tagged chunk of data to the FILE*.
// tagId specifies what to write.
void tag_write(tag_type tagID, writer &outf)
{
    vector<unsigned char> buf;
    writer th(&buf);
    switch (tagID)
    {
    case TAG_CHR:
        tag_construct_char(th);
        break;
    case TAG_YOU:
        tag_construct_you(th);
        CANARY;
        tag_construct_you_items(th);
        CANARY;
        tag_construct_you_dungeon(th);
        CANARY;
        tag_construct_lost_monsters(th);
        CANARY;
        tag_construct_lost_items(th);
        CANARY;
        tag_construct_companions(th);
        break;
    case TAG_LEVEL:
        tag_construct_level(th);
        CANARY;
        tag_construct_level_items(th);
        CANARY;
        tag_construct_level_monsters(th);
        CANARY;
        tag_construct_level_tiles(th);
        break;
    case TAG_GHOST:
        tag_construct_ghost(th);
        break;
    default:
        // I don't know how to make that!
        break;
    }

    // make sure there is some data to write!
    if (buf.empty())
        return;

    // Write tag header.
    marshallInt(outf, buf.size());

    // Write tag data.
    outf.write(&buf[0], buf.size());
}

static void _shunt_monsters_out_of_walls()
{
    for (int i = 0; i < MAX_MONSTERS; ++i)
    {
        monster &m(menv[i]);
        if (m.alive() && in_bounds(m.pos()) && cell_is_solid(m.pos())
            && (grd(m.pos()) != DNGN_MALIGN_GATEWAY
                || mons_genus(m.type) != MONS_ELDRITCH_TENTACLE))
        {
            for (distance_iterator di(m.pos()); di; ++di)
                if (!actor_at(*di) && !cell_is_solid(*di))
                {
#if TAG_MAJOR_VERSION == 34
                    // Could have been a rock worm or a dryad.
                    if (m.type != MONS_GHOST)
#endif
                    mprf(MSGCH_ERROR, "Error: monster %s in %s at (%d,%d)",
                         m.name(DESC_PLAIN, true).c_str(),
                         dungeon_feature_name(grd(m.pos())),
                         m.pos().x, m.pos().y);
                    env.mgrid(m.pos()) = NON_ENTITY;
                    m.position = *di;
                    env.mgrid(*di) = i;
                    break;
                }
        }
    }
}

// Read a piece of data from inf into memory, then run the appropriate reader.
//
// minorVersion is available for any sub-readers that need it
void tag_read(reader &inf, tag_type tag_id)
{
    // Read header info and data
    vector<unsigned char> buf;
    const int data_size = unmarshallInt(inf);
    ASSERT(data_size >= 0);

    // Fetch data in one go
    buf.resize(data_size);
    inf.read(&buf[0], buf.size());

    // Ok, we have data now.
    reader th(buf, inf.getMinorVersion());
    switch (tag_id)
    {
    case TAG_YOU:
        tag_read_you(th);
        EAT_CANARY;
        tag_read_you_items(th);
        EAT_CANARY;
        tag_read_you_dungeon(th);
        EAT_CANARY;
        tag_read_lost_monsters(th);
        EAT_CANARY;
        tag_read_lost_items(th);
        EAT_CANARY;
#if TAG_MAJOR_VERSION == 34
        if (th.getMinorVersion() >= TAG_MINOR_COMPANION_LIST)
#endif
        tag_read_companions(th);

        // If somebody SIGHUP'ed out of the skill menu with all skills disabled.
        // Doing this here rather in tag_read_you() because you.can_train()
        // requires the player's equipment be loaded.
        init_can_train();
        check_selected_skills();
        break;
    case TAG_LEVEL:
        tag_read_level(th);
        EAT_CANARY;
        tag_read_level_items(th);
        EAT_CANARY;
        tag_read_level_monsters(th);
        EAT_CANARY;
        // We have to do this here because tag_read_level_tiles() can
        // call into Lua, which can look at the items ...
        link_items();
#if TAG_MAJOR_VERSION == 34
        _add_missing_branches();
#endif
        _shunt_monsters_out_of_walls();
        // The Abyss needs to visit other levels during level gen, before
        // all cells have been filled. We mustn't crash when it returns
        // from those excursions, and generate_abyss will check_map_validity
        // itself after the grid is fully populated.
        if (you.where_are_you != BRANCH_ABYSS)
        {
            unwind_var<coord_def> you_pos(you.position, coord_def());
            check_map_validity();
        }
        tag_read_level_tiles(th);
        break;
    case TAG_GHOST:
        tag_read_ghost(th);
        break;
    default:
        // I don't know how to read that!
        die("unknown tag type");
    }
}

static void tag_construct_char(writer &th)
{
    marshallByte(th, TAG_CHR_FORMAT);
    // Important: you may never remove or alter a field without bumping
    // CHR_FORMAT.  Bumping it makes all saves invisible when browsed in an
    // older version.
    // Please keep this compatible even over major version breaks!

    // Appending fields is fine.

    marshallString2(th, you.your_name);
    marshallString2(th, Version::Long);

    marshallByte(th, you.species);
    marshallByte(th, you.char_class);
    marshallByte(th, you.experience_level);
    marshallString2(th, you.class_name);
    marshallByte(th, you.religion);
    marshallString2(th, you.jiyva_second_name);

    marshallByte(th, you.wizard);

    marshallByte(th, crawl_state.type);
    if (crawl_state.game_is_tutorial())
        marshallString2(th, crawl_state.map);

    marshallString2(th, species_name(you.species));
    marshallString2(th, you.religion ? god_name(you.religion) : "");

    // separate from the tutorial so we don't have to bump TAG_CHR_FORMAT
    marshallString2(th, crawl_state.map);
}

static void tag_construct_you(writer &th)
{
    int i, j;

    marshallInt(th, you.last_mid);
    marshallByte(th, you.piety);
    marshallByte(th, you.rotting);
    marshallShort(th, you.pet_target);

    marshallByte(th, you.max_level);
    marshallByte(th, you.where_are_you);
    marshallByte(th, you.depth);
    marshallByte(th, you.char_direction);
    marshallByte(th, you.opened_zot);
    marshallByte(th, you.royal_jelly_dead);
    marshallByte(th, you.transform_uncancellable);
    marshallByte(th, you.is_undead);
    marshallByte(th, you.berserk_penalty);
    marshallInt(th, you.abyss_speed);

    marshallInt(th, you.disease);
    ASSERT(you.hp > 0 || you.dead);
    marshallShort(th, you.dead ? 0 : you.hp);

    marshallShort(th, you.hunger);
    marshallBoolean(th, you.fishtail);
    marshallInt(th, you.form);
    CANARY;

    // how many you.equip?
    marshallByte(th, NUM_EQUIP);
    for (i = 0; i < NUM_EQUIP; ++i)
        marshallByte(th, you.equip[i]);
    for (i = 0; i < NUM_EQUIP; ++i)
        marshallBoolean(th, you.melded[i]);

    marshallByte(th, you.magic_points);
    marshallByte(th, you.max_magic_points);

    COMPILE_CHECK(NUM_STATS == 3);
    for (i = 0; i < NUM_STATS; ++i)
        marshallByte(th, you.base_stats[i]);
    for (i = 0; i < NUM_STATS; ++i)
        marshallByte(th, you.stat_loss[i]);
    for (i = 0; i < NUM_STATS; ++i)
        marshallUByte(th, you.stat_zero[i]);

    CANARY;

    marshallByte(th, you.hit_points_regeneration);
    marshallByte(th, you.magic_points_regeneration);

    marshallShort(th, you.hit_points_regeneration * 100);
    marshallInt(th, you.experience);
    marshallInt(th, you.total_experience);
    marshallInt(th, you.gold);

    marshallInt(th, you.exp_available);
    marshallInt(th, you.zot_points);

    marshallInt(th, you.zigs_completed);
    marshallByte(th, you.zig_max);

    marshallShort(th, you.hp_max_adj_temp);
    marshallShort(th, you.hp_max_adj_perm);
    marshallShort(th, you.mp_max_adj);

    marshallShort(th, you.pos().x);
    marshallShort(th, you.pos().y);

    // how many spells?
    marshallUByte(th, MAX_KNOWN_SPELLS);
    for (i = 0; i < MAX_KNOWN_SPELLS; ++i)
        marshallShort(th, you.spells[i]);

    marshallByte(th, 52);
    for (i = 0; i < 52; i++)
        marshallByte(th, you.spell_letter_table[i]);

    marshallByte(th, 52);
    for (i = 0; i < 52; i++)
        marshallShort(th, you.ability_letter_table[i]);

    marshallUByte(th, you.old_vehumet_gifts.size());
    for (set<spell_type>::iterator it = you.old_vehumet_gifts.begin();
         it != you.old_vehumet_gifts.end(); ++it)
    {
        marshallShort(th, *it);
    }

    marshallUByte(th, you.vehumet_gifts.size());
    for (set<spell_type>::iterator it = you.vehumet_gifts.begin();
         it != you.vehumet_gifts.end(); ++it)
    {
        marshallShort(th, *it);
    }

    CANARY;

    // how many skills?
    marshallByte(th, NUM_SKILLS);
    for (j = 0; j < NUM_SKILLS; ++j)
    {
        marshallUByte(th, you.skills[j]);
        marshallByte(th, you.train[j]);
        marshallByte(th, you.train_alt[j]);
        marshallInt(th, you.training[j]);
        marshallInt(th, you.skill_points[j]);
        marshallInt(th, you.ct_skill_points[j]);
        marshallByte(th, you.skill_order[j]);   // skills ordering
    }

    marshallBoolean(th, you.auto_training);
    marshallByte(th, you.exercises.size());
    for (list<skill_type>::iterator it = you.exercises.begin();
         it != you.exercises.end(); ++it)
    {
        marshallInt(th, *it);
    }

    marshallByte(th, you.exercises_all.size());
    for (list<skill_type>::iterator it = you.exercises_all.begin();
         it != you.exercises_all.end(); ++it)
    {
        marshallInt(th, *it);
    }

    marshallByte(th, you.skill_menu_do);
    marshallByte(th, you.skill_menu_view);

    marshallInt(th, you.transfer_from_skill);
    marshallInt(th, you.transfer_to_skill);
    marshallInt(th, you.transfer_skill_points);
    marshallInt(th, you.transfer_total_skill_points);

    CANARY;

    // how many durations?
    marshallByte(th, NUM_DURATIONS);
    for (j = 0; j < NUM_DURATIONS; ++j)
        marshallInt(th, you.duration[j]);

    // how many attributes?
    marshallByte(th, NUM_ATTRIBUTES);
    for (j = 0; j < NUM_ATTRIBUTES; ++j)
        marshallInt(th, you.attribute[j]);

    // Event timers.
    marshallByte(th, NUM_TIMERS);
    for (j = 0; j < NUM_TIMERS; ++j)
    {
        marshallInt(th, you.last_timer_effect[j]);
        marshallInt(th, you.next_timer_effect[j]);
    }

    // how many mutations/demon powers?
    marshallShort(th, NUM_MUTATIONS);
    for (j = 0; j < NUM_MUTATIONS; ++j)
    {
        marshallByte(th, you.mutation[j]);
        marshallByte(th, you.innate_mutation[j]);
        marshallByte(th, you.temp_mutation[j]);
    }

    marshallByte(th, you.demonic_traits.size());
    for (j = 0; j < int(you.demonic_traits.size()); ++j)
    {
        marshallByte(th, you.demonic_traits[j].level_gained);
        marshallShort(th, you.demonic_traits[j].mutation);
    }

    CANARY;

    // how many penances?
    marshallByte(th, NUM_GODS);
    for (i = 0; i < NUM_GODS; i++)
        marshallByte(th, you.penance[i]);

    // which gods have been worshipped by this character?
    for (i = 0; i < NUM_GODS; i++)
        marshallByte(th, you.worshipped[i]);

    // what is the extent of divine generosity?
    for (i = 0; i < NUM_GODS; i++)
        marshallShort(th, you.num_current_gifts[i]);
    for (i = 0; i < NUM_GODS; i++)
        marshallShort(th, you.num_total_gifts[i]);
    for (i = 0; i < NUM_GODS; i++)
        marshallBoolean(th, you.one_time_ability_used[i]);

    // how much piety have you achieved at highest with each god?
    for (i = 0; i < NUM_GODS; i++)
        marshallByte(th, you.piety_max[i]);

    marshallByte(th, you.gift_timeout);

    marshallInt(th, you.exp_docked);
    marshallInt(th, you.exp_docked_total);

    // elapsed time
    marshallInt(th, you.elapsed_time);

    // time of game start
    marshallInt(th, you.birth_time);

    handle_real_time();

    marshallInt(th, you.real_time);
    marshallInt(th, you.num_turns);
    marshallInt(th, you.exploration);

    marshallInt(th, you.magic_contamination);

#if TAG_MAJOR_VERSION == 34
    marshallUByte(th, 0);
#endif
    marshallUByte(th, you.transit_stair);
    marshallByte(th, you.entering_level);
    marshallBoolean(th, you.travel_ally_pace);

    marshallByte(th, you.deaths);
    marshallByte(th, you.lives);

#if TAG_MAJOR_VERSION == 34
    marshallFloat(th, you.temperature);
    marshallFloat(th, you.temperature_last);
#endif

    CANARY;

    marshallInt(th, you.dactions.size());
    for (unsigned int k = 0; k < you.dactions.size(); k++)
        marshallByte(th, you.dactions[k]);

    marshallInt(th, you.level_stack.size());
    for (unsigned int k = 0; k < you.level_stack.size(); k++)
        you.level_stack[k].save(th);

    // List of currently beholding monsters (usually empty).
    marshallShort(th, you.beholders.size());
    for (unsigned int k = 0; k < you.beholders.size(); k++)
         marshallShort(th, you.beholders[k]);

    marshallShort(th, you.fearmongers.size());
    for (unsigned int k = 0; k < you.fearmongers.size(); k++)
        marshallShort(th, you.fearmongers[k]);

    marshallByte(th, you.piety_hysteresis);

    you.m_quiver->save(th);

    marshallString(th, you.zotdef_wave_name);

    CANARY;

    // Action counts.
    j = 0;
    for (map<pair<caction_type, int>, FixedVector<int, 27> >::const_iterator ac =
         you.action_count.begin(); ac != you.action_count.end(); ++ac)
    {
        j++;
    }
    marshallShort(th, j);
    for (map<pair<caction_type, int>, FixedVector<int, 27> >::const_iterator ac =
         you.action_count.begin(); ac != you.action_count.end(); ++ac)
    {
        marshallShort(th, ac->first.first);
        marshallInt(th, ac->first.second);
        for (int k = 0; k < 27; k++)
            marshallInt(th, ac->second[k]);
    }

    marshallByte(th, NUM_BRANCHES);
    for (i = 0; i < NUM_BRANCHES; i++)
        marshallBoolean(th, you.branches_left[i]);

    marshallCoord(th, abyssal_state.major_coord);
    marshallInt(th, abyssal_state.seed);
    marshallInt(th, abyssal_state.depth);
    marshallFloat(th, abyssal_state.phase);
    marshall_level_id(th, abyssal_state.level);

#if TAG_MAJOR_VERSION == 34
    if (abyssal_state.level.branch == BRANCH_DWARF || !abyssal_state.level.is_valid())
        abyssal_state.level = level_id(static_cast<branch_type>(BRANCH_DUNGEON), 19);
#endif

    _marshall_constriction(th, &you);

    marshallUByte(th, you.octopus_king_rings);

    marshallUnsigned(th, you.uncancel.size());
    for (i = 0; i < (int)you.uncancel.size(); i++)
    {
        marshallUByte(th, you.uncancel[i].first);
        marshallInt(th, you.uncancel[i].second);
    }

    marshallUnsigned(th, you.recall_list.size());
    for (i = 0; i < (int)you.recall_list.size(); i++)
        _marshall_as_int<mid_t>(th, you.recall_list[i]);

    marshallUByte(th, NUM_SEEDS);
    for (i = 0; i < NUM_SEEDS; i++)
        marshallInt(th, you.game_seeds[i]);

    CANARY;

    if (!dlua.callfn("dgn_save_data", "u", &th))
        mprf(MSGCH_ERROR, "Failed to save Lua data: %s", dlua.error.c_str());

    CANARY;

    // Write a human-readable string out on the off chance that
    // we fail to be able to read this file back in using some later version.
    string revision = "Git:";
    revision += Version::Long;
    marshallString(th, revision);

    you.props.write(th);
}

static void tag_construct_you_items(writer &th)
{
    int i,j;

    // how many inventory slots?
    marshallByte(th, ENDOFPACK);
    for (i = 0; i < ENDOFPACK; ++i)
        marshallItem(th, you.inv[i]);

    marshallFixedBitVector<NUM_RUNE_TYPES>(th, you.runes);
    marshallByte(th, you.obtainable_runes);

    // Item descrip for each type & subtype.
    // how many types?
    marshallUByte(th, NUM_IDESC);
    // how many subtypes?
    marshallUByte(th, MAX_SUBTYPES);
    for (i = 0; i < NUM_IDESC; ++i)
        for (j = 0; j < MAX_SUBTYPES; ++j)
            marshallByte(th, you.item_description[i][j]);

    marshallUByte(th, NUM_OBJECT_CLASSES);
    for (i = 0; i < NUM_OBJECT_CLASSES; ++i)
    {
        if (!item_type_has_ids((object_class_type)i))
            continue;
        for (j = 0; j < MAX_SUBTYPES; ++j)
            marshallUByte(th, you.type_ids[i][j]);
    }

    // Additional identification info
    you.type_id_props.write(th);

    CANARY;

    // how many unique items?
    marshallUByte(th, MAX_UNRANDARTS);
    for (j = 0; j < MAX_UNRANDARTS; ++j)
        marshallByte(th,you.unique_items[j]);

    marshallByte(th, NUM_FIXED_BOOKS);
    for (j = 0; j < NUM_FIXED_BOOKS; ++j)
        marshallByte(th,you.had_book[j]);

    marshallShort(th, NUM_SPELLS);
    for (j = 0; j < NUM_SPELLS; ++j)
        marshallByte(th,you.seen_spell[j]);

    marshallShort(th, NUM_WEAPONS);
    for (j = 0; j < NUM_WEAPONS; ++j)
        marshallInt(th,you.seen_weapon[j]);

    marshallShort(th, NUM_ARMOURS);
    for (j = 0; j < NUM_ARMOURS; ++j)
        marshallInt(th,you.seen_armour[j]);

    marshallFixedBitVector<NUM_MISCELLANY>(th, you.seen_misc);

    for (i = 0; i < NUM_OBJECT_CLASSES; i++)
        for (j = 0; j < MAX_SUBTYPES; j++)
            marshallInt(th, you.force_autopickup[i][j]);
}

static void marshallPlaceInfo(writer &th, PlaceInfo place_info)
{
    marshallInt(th, place_info.branch);

    marshallInt(th, place_info.num_visits);
    marshallInt(th, place_info.levels_seen);

    marshallInt(th, place_info.mon_kill_exp);

    for (int i = 0; i < KC_NCATEGORIES; i++)
        marshallInt(th, place_info.mon_kill_num[i]);

    marshallInt(th, place_info.turns_total);
    marshallInt(th, place_info.turns_explore);
    marshallInt(th, place_info.turns_travel);
    marshallInt(th, place_info.turns_interlevel);
    marshallInt(th, place_info.turns_resting);
    marshallInt(th, place_info.turns_other);

    marshallInt(th, place_info.elapsed_total);
    marshallInt(th, place_info.elapsed_explore);
    marshallInt(th, place_info.elapsed_travel);
    marshallInt(th, place_info.elapsed_interlevel);
    marshallInt(th, place_info.elapsed_resting);
    marshallInt(th, place_info.elapsed_other);
}

static void tag_construct_you_dungeon(writer &th)
{
    // how many unique creatures?
    marshallShort(th, NUM_MONSTERS);
    for (int j = 0; j < NUM_MONSTERS; ++j)
        marshallByte(th,you.unique_creatures[j]); // unique beasties

    // how many branches?
    marshallByte(th, NUM_BRANCHES);
    for (int j = 0; j < NUM_BRANCHES; ++j)
    {
        marshallInt(th, brdepth[j]);
        marshall_level_id(th, brentry[j]);
        marshallInt(th, branch_bribe[j]);
    }

    // Root of the dungeon; usually BRANCH_DUNGEON.
    marshallInt(th, root_branch);

    marshallMap(th, stair_level,
                _marshall_as_int<branch_type>, _marshall_level_id_set);
    marshallMap(th, shops_present,
                _marshall_level_pos, _marshall_as_int<shop_type>);
    marshallMap(th, altars_present,
                _marshall_level_pos, _marshall_as_int<god_type>);
    marshallMap(th, portals_present,
                _marshall_level_pos, _marshall_as_int<branch_type>);
    marshallMap(th, portal_notes,
                _marshall_level_pos, marshallString);
    marshallMap(th, level_annotations,
                marshall_level_id, marshallString);
    marshallMap(th, level_exclusions,
                marshall_level_id, marshallString);
    marshallMap(th, level_uniques,
            marshall_level_id, marshallString);
    marshallUniqueAnnotations(th);

    marshallPlaceInfo(th, you.global_info);
    vector<PlaceInfo> list = you.get_all_place_info();
    // How many different places we have info on?
    marshallShort(th, list.size());

    for (unsigned int k = 0; k < list.size(); k++)
        marshallPlaceInfo(th, list[k]);

    marshall_iterator(th, you.uniq_map_tags.begin(), you.uniq_map_tags.end(),
                      marshallString);
    marshall_iterator(th, you.uniq_map_names.begin(), you.uniq_map_names.end(),
                      marshallString);
    marshallMap(th, you.vault_list, marshall_level_id, marshallStringVector);

    write_level_connectivity(th);
}

static void marshall_follower(writer &th, const follower &f)
{
    ASSERT(!invalid_monster_type(f.mons.type));
    ASSERT(f.mons.alive());
    marshallMonster(th, f.mons);
    for (int i = 0; i < NUM_MONSTER_SLOTS; ++i)
        marshallItem(th, f.items[i]);
}

static follower unmarshall_follower(reader &th)
{
    follower f;
    unmarshallMonster(th, f.mons);
    for (int i = 0; i < NUM_MONSTER_SLOTS; ++i)
        unmarshallItem(th, f.items[i]);
    return f;
}

static void marshall_companion(writer &th, const companion &c)
{
    marshall_follower(th, c.mons);
    marshall_level_id(th, c.level);
    marshallInt(th, c.timestamp);
}

static companion unmarshall_companion(reader &th)
{
    companion c;
    c.mons = unmarshall_follower(th);
    c.level = unmarshall_level_id(th);
    c.timestamp = unmarshallInt(th);
    return c;
}

static void marshall_follower_list(writer &th, const m_transit_list &mlist)
{
    marshallShort(th, mlist.size());

    for (m_transit_list::const_iterator mi = mlist.begin();
         mi != mlist.end(); ++mi)
    {
        marshall_follower(th, *mi);
    }
}

static void marshall_item_list(writer &th, const i_transit_list &ilist)
{
    marshallShort(th, ilist.size());

    for (i_transit_list::const_iterator ii = ilist.begin();
         ii != ilist.end(); ++ii)
    {
        marshallItem(th, *ii);
    }
}

static m_transit_list unmarshall_follower_list(reader &th)
{
    m_transit_list mlist;

    const int size = unmarshallShort(th);

    for (int i = 0; i < size; ++i)
    {
        follower f = unmarshall_follower(th);
        if (!f.mons.alive())
        {
            mprf(MSGCH_ERROR,
                 "Dead monster %s in transit list in saved game, ignoring.",
                 f.mons.name(DESC_PLAIN, true).c_str());
        }
        else
            mlist.push_back(f);
    }

    return mlist;
}

static i_transit_list unmarshall_item_list(reader &th)
{
    i_transit_list ilist;

    const int size = unmarshallShort(th);

    for (int i = 0; i < size; ++i)
    {
        item_def item;
        unmarshallItem(th, item);
        ilist.push_back(item);
    }

    return ilist;
}

static void marshall_level_map_masks(writer &th)
{
    for (rectangle_iterator ri(0); ri; ++ri)
    {
        marshallInt(th, env.level_map_mask(*ri));
        marshallInt(th, env.level_map_ids(*ri));
    }
}

static void unmarshall_level_map_masks(reader &th)
{
    for (rectangle_iterator ri(0); ri; ++ri)
    {
        env.level_map_mask(*ri) = unmarshallInt(th);
        env.level_map_ids(*ri)  = unmarshallInt(th);
    }
}

static void marshall_level_map_unique_ids(writer &th)
{
    marshallSet(th, env.level_uniq_maps, marshallString);
    marshallSet(th, env.level_uniq_map_tags, marshallString);
}

static void unmarshall_level_map_unique_ids(reader &th)
{
    unmarshallSet(th, env.level_uniq_maps, unmarshallString);
    unmarshallSet(th, env.level_uniq_map_tags, unmarshallString);
}

static void marshall_subvault_place(writer &th,
                                    const subvault_place &subvault_place);

static void marshall_mapdef(writer &th, const map_def &map)
{
    marshallString(th, map.name);
    map.write_index(th);
    map.write_maplines(th);
    marshallString(th, map.description);
    marshallMap(th, map.feat_renames,
                _marshall_as_int<dungeon_feature_type>, marshallString);
    marshall_iterator(th,
                      map.subvault_places.begin(),
                      map.subvault_places.end(),
                      marshall_subvault_place);
}

static void marshall_subvault_place(writer &th,
                                    const subvault_place &subvault_place)
{
    marshallCoord(th, subvault_place.tl);
    marshallCoord(th, subvault_place.br);
    marshall_mapdef(th, *subvault_place.subvault);
}

static subvault_place unmarshall_subvault_place(reader &th);
static map_def unmarshall_mapdef(reader &th)
{
    map_def map;
    map.name = unmarshallString(th);
    map.read_index(th);
    map.read_maplines(th);
    map.description = unmarshallString(th);
    unmarshallMap(th, map.feat_renames,
                  unmarshall_int_as<dungeon_feature_type>,
                  unmarshallString);
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() >= TAG_MINOR_REIFY_SUBVAULTS
        && th.getMinorVersion() != TAG_MINOR_0_11)
#endif
        unmarshall_vector(th, map.subvault_places, unmarshall_subvault_place);
    return map;
}

static subvault_place unmarshall_subvault_place(reader &th)
{
    subvault_place subvault;
    subvault.tl = unmarshallCoord(th);
    subvault.br = unmarshallCoord(th);
    subvault.set_subvault(unmarshall_mapdef(th));
    return subvault;
}

static void marshall_vault_placement(writer &th, const vault_placement &vp)
{
    marshallCoord(th, vp.pos);
    marshallCoord(th, vp.size);
    marshallShort(th, vp.orient);
    marshall_mapdef(th, vp.map);
    marshall_iterator(th, vp.exits.begin(), vp.exits.end(), marshallCoord);
#if TAG_MAJOR_VERSION == 34
    marshallShort(th, -1);
#endif
    marshallByte(th, vp.seen);
}

static vault_placement unmarshall_vault_placement(reader &th)
{
    vault_placement vp;
    vp.pos = unmarshallCoord(th);
    vp.size = unmarshallCoord(th);
    vp.orient = static_cast<map_section_type>(unmarshallShort(th));
    vp.map = unmarshall_mapdef(th);
    unmarshall_vector(th, vp.exits, unmarshallCoord);
#if TAG_MAJOR_VERSION == 34
    unmarshallShort(th);
#endif
    vp.seen = !!unmarshallByte(th);

    return vp;
}

static void marshall_level_vault_placements(writer &th)
{
    marshallShort(th, env.level_vaults.size());
    for (int i = 0, size = env.level_vaults.size(); i < size; ++i)
        marshall_vault_placement(th, *env.level_vaults[i]);
}

static void unmarshall_level_vault_placements(reader &th)
{
    const int nvaults = unmarshallShort(th);
    ASSERT(nvaults >= 0);
    dgn_clear_vault_placements(env.level_vaults);
    for (int i = 0; i < nvaults; ++i)
    {
        env.level_vaults.push_back(
            new vault_placement(unmarshall_vault_placement(th)));
    }
}

static void marshall_level_vault_data(writer &th)
{
    marshallString(th, env.level_build_method);
    marshallSet(th, env.level_layout_types, marshallString);

    marshall_level_map_masks(th);
    marshall_level_map_unique_ids(th);
#if TAG_MAJOR_VERSION == 34
    marshallInt(th, 0);
#endif
    marshall_level_vault_placements(th);
}

static void unmarshall_level_vault_data(reader &th)
{
    env.level_build_method = unmarshallString(th);
    unmarshallSet(th, env.level_layout_types, unmarshallString);

    unmarshall_level_map_masks(th);
    unmarshall_level_map_unique_ids(th);
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() >= TAG_MINOR_VAULT_LIST) // 33:17 has it
        unmarshallStringVector(th);
#endif
    unmarshall_level_vault_placements(th);
}

static void tag_construct_lost_monsters(writer &th)
{
    marshallMap(th, the_lost_ones, marshall_level_id,
                 marshall_follower_list);
}

static void tag_construct_lost_items(writer &th)
{
    marshallMap(th, transiting_items, marshall_level_id,
                 marshall_item_list);
}

static void tag_construct_companions(writer &th)
{
#if TAG_MAJOR_VERSION == 34
    fixup_bad_companions();
#endif
    marshallMap(th, companion_list, _marshall_as_int<mid_t>,
                 marshall_companion);
}

// Save versions 30-32.26 are readable but don't store the names.
static const char* old_species[]=
{
    "Human", "High Elf", "Deep Elf", "Sludge Elf", "Mountain Dwarf", "Halfling",
    "Hill Orc", "Kobold", "Mummy", "Naga", "Ogre", "Troll",
    "Red Draconian", "White Draconian", "Green Draconian", "Yellow Draconian",
    "Grey Draconian", "Black Draconian", "Purple Draconian", "Mottled Draconian",
    "Pale Draconian", "Draconian", "Centaur", "Demigod", "Spriggan", "Minotaur",
    "Demonspawn", "Ghoul", "Tengu", "Merfolk", "Vampire", "Deep Dwarf", "Felid",
    "Octopode",
};

static const char* old_gods[]=
{
    "", "Zin", "The Shining One", "Kikubaaqudgha", "Yredelemnul", "Xom",
    "Vehumet", "Okawaru", "Makhleb", "Sif Muna", "Trog", "Nemelex Xobeh",
    "Elyvilon", "Lugonu", "Beogh", "Jiyva", "Fedhas", "Cheibriados",
    "Ashenzari",
};

void tag_read_char(reader &th, uint8_t format, uint8_t major, uint8_t minor)
{
    // Important: values out of bounds are good here, the save browser needs to
    // be forward-compatible.  We validate them only on an actual restore.
    you.your_name         = unmarshallString2(th);
    you.prev_save_version = unmarshallString2(th);
    dprf("Last save Crawl version: %s", you.prev_save_version.c_str());

    you.species           = static_cast<species_type>(unmarshallUByte(th));
    you.char_class        = static_cast<job_type>(unmarshallUByte(th));
    you.experience_level  = unmarshallByte(th);
    you.class_name        = unmarshallString2(th);
    you.religion          = static_cast<god_type>(unmarshallUByte(th));
    you.jiyva_second_name = unmarshallString2(th);

    you.wizard            = unmarshallBoolean(th);

    crawl_state.type = (game_type) unmarshallUByte(th);
    if (crawl_state.game_is_tutorial())
        crawl_state.map = unmarshallString2(th);
    else
        crawl_state.map = "";

    if (major > 32 || major == 32 && minor > 26)
    {
        you.species_name = unmarshallString2(th);
        you.god_name     = unmarshallString2(th);
    }
    else
    {
        if (you.species >= 0 && you.species < (int)ARRAYSZ(old_species))
            you.species_name = old_species[you.species];
        else
            you.species_name = "Yak";
        if (you.religion >= 0 && you.religion < (int)ARRAYSZ(old_gods))
            you.god_name = old_gods[you.religion];
        else
            you.god_name = "Marduk";
    }

    if (major > 34 || major == 34 && minor >= 29)
        crawl_state.map = unmarshallString2(th);
}

static void tag_read_you(reader &th)
{
    int i,j;
    int count;

    ASSERT(is_valid_species(you.species));
    ASSERT(you.char_class < NUM_JOBS);
    ASSERT_RANGE(you.experience_level, 1, 28);
    ASSERT(you.religion < NUM_GODS);
    ASSERT_RANGE(crawl_state.type, GAME_TYPE_UNSPECIFIED + 1, NUM_GAME_TYPE);
    you.last_mid          = unmarshallInt(th);
    you.piety             = unmarshallUByte(th);
    ASSERT(you.piety <= MAX_PIETY);
    you.rotting           = unmarshallUByte(th);
    you.pet_target        = unmarshallShort(th);

    you.max_level         = unmarshallByte(th);
    you.where_are_you     = static_cast<branch_type>(unmarshallUByte(th));
    ASSERT(you.where_are_you < NUM_BRANCHES);
    you.depth             = unmarshallByte(th);
    ASSERT(you.depth > 0);
    you.char_direction    = static_cast<game_direction_type>(unmarshallUByte(th));
    ASSERT(you.char_direction <= GDT_ASCENDING);

    you.opened_zot = unmarshallBoolean(th);
    you.royal_jelly_dead = unmarshallBoolean(th);
    you.transform_uncancellable = unmarshallBoolean(th);

    you.is_undead         = static_cast<undead_state_type>(unmarshallUByte(th));
    ASSERT(you.is_undead <= US_SEMI_UNDEAD);
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_CALC_UNRAND_REACTS)
      unmarshallShort(th);
#endif
    you.berserk_penalty   = unmarshallByte(th);
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() >= TAG_MINOR_GARGOYLE_DR
      && th.getMinorVersion() < TAG_MINOR_RM_GARGOYLE_DR)
    {
        unmarshallInt(th); // Slough an integer.
    }

    if (th.getMinorVersion() < TAG_MINOR_AUTOMATIC_MANUALS)
    {
        unmarshallShort(th);
        unmarshallInt(th);
    }
#endif

    you.abyss_speed = unmarshallInt(th);

    you.disease         = unmarshallInt(th);
    you.hp              = unmarshallShort(th);
    you.hunger          = unmarshallShort(th);
    you.fishtail        = unmarshallBoolean(th);
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_NOME_NO_MORE)
        unmarshallInt(th);
#endif
    you.form            = static_cast<transformation_type>(unmarshallInt(th));
    ASSERT_RANGE(you.form, TRAN_NONE, LAST_FORM + 1);
#if TAG_MAJOR_VERSION == 34
    // Fix the effects of #7668 (Vampire lose undead trait once coming back
    // from lich form).
    if (you.species == SP_VAMPIRE && you.form != TRAN_LICH)
        you.is_undead = US_SEMI_UNDEAD;

    if (you.form == TRAN_NONE)
        you.transform_uncancellable = false;
#else
    ASSERT(you.form != TRAN_NONE || !you.transform_uncancellable);
#endif
    EAT_CANARY;

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_SAGE_REMOVAL)
    {
        count = unmarshallShort(th);
        ASSERT_RANGE(count, 0, 32768);
        for (i = 0; i < count; ++i)
        {
            unmarshallByte(th);
            unmarshallInt(th);
            unmarshallInt(th);
        }
    }
#endif

    // How many you.equip?
    count = unmarshallByte(th);
    ASSERT(count <= NUM_EQUIP);
    for (i = 0; i < count; ++i)
    {
        you.equip[i] = unmarshallByte(th);
        ASSERT_RANGE(you.equip[i], -1, ENDOFPACK);
    }
    for (i = count; i < NUM_EQUIP; ++i)
        you.equip[i] = -1;
    for (i = 0; i < count; ++i)
        you.melded.set(i, unmarshallBoolean(th));
    for (i = count; i < NUM_EQUIP; ++i)
        you.melded.set(i, false);

    you.magic_points              = unmarshallByte(th);
    you.max_magic_points          = unmarshallByte(th);

    for (i = 0; i < NUM_STATS; ++i)
        you.base_stats[i] = unmarshallByte(th);
    for (i = 0; i < NUM_STATS; ++i)
        you.stat_loss[i] = unmarshallByte(th);
    for (i = 0; i < NUM_STATS; ++i)
        you.stat_zero[i] = unmarshallUByte(th);
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_STAT_ZERO)
    {
        for (i = 0; i < NUM_STATS; ++i)
            unmarshallString(th);
    }
#endif
    EAT_CANARY;

    you.hit_points_regeneration   = unmarshallByte(th);
    you.magic_points_regeneration = unmarshallByte(th);

    you.hit_points_regeneration   = unmarshallShort(th) / 100;
    you.experience                = unmarshallInt(th);
    you.total_experience = unmarshallInt(th);
    you.gold                      = unmarshallInt(th);
    you.exp_available             = unmarshallInt(th);
    you.zot_points                = unmarshallInt(th);
    you.zigs_completed            = unmarshallInt(th);
    you.zig_max                   = unmarshallByte(th);

    you.hp_max_adj_temp           = unmarshallShort(th);
    you.hp_max_adj_perm           = unmarshallShort(th);
    you.mp_max_adj                = unmarshallShort(th);
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_REMOVE_BASE_MP)
    {
        int baseadj = unmarshallShort(th);
        you.mp_max_adj += baseadj;
    }
    if (th.getMinorVersion() < TAG_MINOR_CLASS_HP_0)
        you.hp_max_adj_perm -= 8;
#endif

    const int x = unmarshallShort(th);
    const int y = unmarshallShort(th);
    // SIGHUP during Step from Time/etc is ok.
    ASSERT(!x && !y || in_bounds(x, y));
    you.moveto(coord_def(x, y));

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_WEIGHTLESS)
        unmarshallShort(th);
#endif

    // how many spells?
    you.spell_no = 0;
    count = unmarshallUByte(th);
    ASSERT(count >= 0);
    for (i = 0; i < count && i < MAX_KNOWN_SPELLS; ++i)
    {
        you.spells[i] = unmarshallSpellType(th);
        if (you.spells[i] != SPELL_NO_SPELL)
            you.spell_no++;
    }
    for (; i < count; ++i)
    {
#if TAG_MAJOR_VERSION == 34
        if (th.getMinorVersion() < TAG_MINOR_SHORT_SPELL_TYPE)
            unmarshallUByte(th);
        else
#endif
            unmarshallShort(th);
    }

    count = unmarshallByte(th);
    ASSERT(count == (int)you.spell_letter_table.size());
    for (i = 0; i < count; i++)
    {
        int s = unmarshallByte(th);
        ASSERT_RANGE(s, -1, MAX_KNOWN_SPELLS);
        you.spell_letter_table[i] = s;
    }

    count = unmarshallByte(th);
    ASSERT(count == (int)you.ability_letter_table.size());
#if TAG_MAJOR_VERSION == 34
    bool found_fly = false;
    bool found_stop_flying = false;
#endif
    for (i = 0; i < count; i++)
    {
        int a = unmarshallShort(th);
        ASSERT_RANGE(a, -1, NUM_ABILITIES);
        ASSERT(a != 0);
#if TAG_MAJOR_VERSION == 34
        if (th.getMinorVersion() < TAG_MINOR_ABIL_1000)
        {
            if (a >= 230)
                a += 2000 - 230;
            else if (a >= 50)
                a += 1000 - 50;
        }
        if (th.getMinorVersion() < TAG_MINOR_ABIL_GOD_FIXUP)
        {
            if (a >= ABIL_ASHENZARI_END_TRANSFER + 1
                && a <= ABIL_ASHENZARI_END_TRANSFER + 3)
            {
                a += ABIL_STOP_RECALL - ABIL_ASHENZARI_END_TRANSFER;
            }
        }
        if (a == ABIL_FLY
            || a == ABIL_WISP_BLINK // was ABIL_FLY_II
               && th.getMinorVersion() < TAG_MINOR_0_12)
        {
            if (found_fly)
                a = ABIL_NON_ABILITY;
            else
                a = ABIL_FLY;
            found_fly = true;
        }
        if (a == ABIL_EVOKE_STOP_LEVITATING
            || a == ABIL_STOP_FLYING)
        {
            if (found_stop_flying)
                a = ABIL_NON_ABILITY;
            else
                a = ABIL_STOP_FLYING;
            found_stop_flying = true;
        }
#endif
        you.ability_letter_table[i] = static_cast<ability_type>(a);
    }

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() >= TAG_MINOR_VEHUMET_SPELL_GIFT
        && th.getMinorVersion() != TAG_MINOR_0_11)
    {
#endif
        count = unmarshallUByte(th);
        for (i = 0; i < count; ++i)
            you.old_vehumet_gifts.insert(unmarshallSpellType(th));

#if TAG_MAJOR_VERSION == 34
        if (th.getMinorVersion() < TAG_MINOR_VEHUMET_MULTI_GIFTS)
            you.vehumet_gifts.insert(unmarshallSpellType(th));
        else
        {
#endif
            count = unmarshallUByte(th);
            for (i = 0; i < count; ++i)
                you.vehumet_gifts.insert(unmarshallSpellType(th));
#if TAG_MAJOR_VERSION == 34
        }
    }
#endif
    EAT_CANARY;

    // how many skills?
    count = unmarshallUByte(th);
    ASSERT(count <= NUM_SKILLS);
    for (j = 0; j < count; ++j)
    {
        you.skills[j]          = unmarshallUByte(th);
        ASSERT(you.skills[j] <= 27 || you.wizard);

        you.train[j]    = unmarshallByte(th);
        you.train_alt[j]    = unmarshallByte(th);
        you.training[j] = unmarshallInt(th);
        you.skill_points[j]    = unmarshallInt(th);
        you.ct_skill_points[j] = unmarshallInt(th);
        you.skill_order[j]     = unmarshallByte(th);
    }

    you.auto_training = unmarshallBoolean(th);

    count = unmarshallByte(th);
    for (i = 0; i < count; i++)
        you.exercises.push_back((skill_type)unmarshallInt(th));

    count = unmarshallByte(th);
    for (i = 0; i < count; i++)
        you.exercises_all.push_back((skill_type)unmarshallInt(th));

    you.skill_menu_do = static_cast<skill_menu_state>(unmarshallByte(th));
    you.skill_menu_view = static_cast<skill_menu_state>(unmarshallByte(th));
    you.transfer_from_skill = static_cast<skill_type>(unmarshallInt(th));
    ASSERT(you.transfer_from_skill == SK_NONE || you.transfer_from_skill < NUM_SKILLS);
    you.transfer_to_skill = static_cast<skill_type>(unmarshallInt(th));
    ASSERT(you.transfer_to_skill == SK_NONE || you.transfer_to_skill < NUM_SKILLS);
    you.transfer_skill_points = unmarshallInt(th);
    you.transfer_total_skill_points = unmarshallInt(th);

    // Set up you.skill_cost_level.
    you.skill_cost_level = 0;
    check_skill_cost_change();

    EAT_CANARY;

    // how many durations?
    count = unmarshallUByte(th);
    COMPILE_CHECK(NUM_DURATIONS < 256);
    for (j = 0; j < count && j < NUM_DURATIONS; ++j)
        you.duration[j] = unmarshallInt(th);
    for (j = NUM_DURATIONS; j < count; ++j)
        unmarshallInt(th);
#if TAG_MAJOR_VERSION == 34
    if (you.species == SP_LAVA_ORC)
        you.duration[DUR_STONESKIN] = 0;

    if (th.getMinorVersion() < TAG_MINOR_FUNGUS_FORM && you.form == TRAN_FUNGUS)
        you.duration[DUR_CONFUSING_TOUCH] = 0;

    you.duration[DUR_JELLY_PRAYER] = 0;
#endif

    // how many attributes?
    count = unmarshallUByte(th);
    COMPILE_CHECK(NUM_ATTRIBUTES < 256);
    for (j = 0; j < count && j < NUM_ATTRIBUTES; ++j)
    {
#if TAG_MAJOR_VERSION == 34
        if (j == ATTR_BANISHMENT_IMMUNITY && th.getMinorVersion() == TAG_MINOR_0_11)
        {
            unmarshallInt(th); // ATTR_UNUSED_1
            count--;
        }
        if (j == ATTR_NOISES && th.getMinorVersion() == TAG_MINOR_CLASS_HP_0
            && count == 40)
        {
            dprf("recovering ATTR_NOISES");
            j++, count++;
        }
#endif
        you.attribute[j] = unmarshallInt(th);
    }
    for (j = count; j < NUM_ATTRIBUTES; ++j)
        you.attribute[j] = 0;
    for (j = NUM_ATTRIBUTES; j < count; ++j)
        unmarshallInt(th);

#if TAG_MAJOR_VERSION == 34
    if (you.attribute[ATTR_DIVINE_REGENERATION])
    {
        you.attribute[ATTR_DIVINE_REGENERATION] = 0;
        you.duration[DUR_TROGS_HAND] = max(you.duration[DUR_TROGS_HAND],
                                           you.duration[DUR_REGENERATION]);
        you.duration[DUR_REGENERATION] = 0;
    }
    if (you.attribute[ATTR_SEARING_RAY] > 3)
        you.attribute[ATTR_SEARING_RAY] = 0;
#endif

#if TAG_MAJOR_VERSION == 34
    // Nemelex item type sacrifice toggles.
    if (th.getMinorVersion() < TAG_MINOR_NEMELEX_WEIGHTS)
    {
        count = unmarshallByte(th);
        ASSERT(count <= NUM_OBJECT_CLASSES);
        for (j = 0; j < count; ++j)
            unmarshallInt(th);
    }
#endif

    int timer_count = 0;
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() >= TAG_MINOR_EVENT_TIMERS)
    {
#endif
    timer_count = unmarshallByte(th);
    ASSERT(timer_count <= NUM_TIMERS);
    for (j = 0; j < timer_count; ++j)
    {
        you.last_timer_effect[j] = unmarshallInt(th);
        you.next_timer_effect[j] = unmarshallInt(th);
    }
#if TAG_MAJOR_VERSION == 34
    }
    else
        timer_count = 0;
#endif
    // We'll have to fix up missing/broken timer entries after
    // we unmarshall you.elapsed_time.

    // how many mutations/demon powers?
    count = unmarshallShort(th);
    ASSERT_RANGE(count, 0, NUM_MUTATIONS + 1);
    for (j = 0; j < count; ++j)
    {
        you.mutation[j]         = unmarshallUByte(th);
        you.innate_mutation[j] = unmarshallUByte(th);
#if TAG_MAJOR_VERSION == 34
        if (th.getMinorVersion() >= TAG_MINOR_TEMP_MUTATIONS
            && th.getMinorVersion() != TAG_MINOR_0_11)
        {
#endif
        you.temp_mutation[j] = unmarshallUByte(th);
#if TAG_MAJOR_VERSION == 34
        }
#endif

#if TAG_MAJOR_VERSION == 34
        if (you.innate_mutation[j] + you.temp_mutation[j] > you.mutation[j])
        {
            mprf(MSGCH_ERROR, "Mutation #%d out of sync, fixing up.", j);
            you.mutation[j] = you.innate_mutation[j] + you.temp_mutation[j];
        }
#endif
    }

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_STAT_MUT)
    {
        // Convert excess mutational stats into base stats.
        mutation_type stat_mutations[] = { MUT_STRONG, MUT_CLEVER, MUT_AGILE };
        stat_type stat_types[] = { STAT_STR, STAT_INT, STAT_DEX };
        for (j = 0; j < 3; ++j)
        {
            mutation_type mut = stat_mutations[j];
            stat_type stat = stat_types[j];
            int total_mutation_level = you.temp_mutation[mut] + you.mutation[mut];
            if (total_mutation_level > 2)
            {
                int new_level = max(0, min(you.temp_mutation[mut] - you.mutation[mut], 2));
                you.temp_mutation[mut] = new_level;
            }
            if (you.mutation[mut] > 2)
            {
                int excess = you.mutation[mut] - 4;
                if (excess > 0)
                    you.base_stats[stat] += excess;
                you.mutation[mut] = 2;
            }
        }
        mutation_type bad_stat_mutations[] = { MUT_WEAK, MUT_DOPEY, MUT_CLUMSY };
        for (j = 0; j < 3; ++j)
        {
            mutation_type mut = bad_stat_mutations[j];
            int level = you.mutation[mut];
            switch (level)
            {
            case 0:
            case 1:
                you.mutation[mut] = 0;
                break;
            case 2:
            case 3:
                you.mutation[mut] = 1;
                break;
            default:
                you.mutation[mut] = 2;
                break;
            };
            if (you.temp_mutation[mut] > 2 && you.mutation[mut] < 2)
                you.temp_mutation[mut] = 1;
            else
                you.temp_mutation[mut] = 0;
        }
    }
    you.mutation[MUT_FAST] = you.innate_mutation[MUT_FAST];
    you.mutation[MUT_SLOW] = you.innate_mutation[MUT_SLOW];
#endif

    for (j = count; j < NUM_MUTATIONS; ++j)
        you.mutation[j] = you.innate_mutation[j] = 0;

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_NO_DEVICE_HEAL)
    {   // These use to apply no matter what the minor tag
        // was, so when TAG_MINOR_NO_DEVICE_HEAL was added
        // these were all moved to only apply to previous
        // tags.
        if (you.mutation[MUT_TELEPORT_CONTROL] == 1)
            you.mutation[MUT_TELEPORT_CONTROL] = 0;
        if (you.mutation[MUT_TRAMPLE_RESISTANCE] > 0)
            you.mutation[MUT_TRAMPLE_RESISTANCE] = 0;
        if (you.mutation[MUT_CLING] == 1)
            you.mutation[MUT_CLING] = 0;
        if (you.species == SP_GARGOYLE)
        {
            you.mutation[MUT_POISON_RESISTANCE] =
            you.innate_mutation[MUT_POISON_RESISTANCE] = 0;
        }
        if (you.species == SP_DJINNI)
        {
            you.mutation[MUT_NEGATIVE_ENERGY_RESISTANCE] =
            you.innate_mutation[MUT_NEGATIVE_ENERGY_RESISTANCE] = 3;
        }
        if (you.species == SP_FELID && you.innate_mutation[MUT_JUMP] == 0)
        {
            you.mutation[MUT_JUMP] = you.innate_mutation[MUT_JUMP]
                                   = min(1 + you.experience_level / 6, 3);
        }
        if (you.species == SP_FORMICID)
        {
            you.mutation[MUT_ANTENNAE] = you.innate_mutation[MUT_ANTENNAE] = 3;
            you.mutation[MUT_EXOSKELETON] =
            you.innate_mutation[MUT_EXOSKELETON] = 0;
        }
    }

    if (th.getMinorVersion() < TAG_MINOR_DIET_MUT)
    {
        you.mutation[MUT_CARNIVOROUS] = you.innate_mutation[MUT_CARNIVOROUS];
        you.mutation[MUT_HERBIVOROUS] = you.innate_mutation[MUT_HERBIVOROUS];

    }

    if (th.getMinorVersion() < TAG_MINOR_SAPROVOROUS)
    {
        if (you.species == SP_LAVA_ORC || you.species == SP_HILL_ORC
            || you.species == SP_OGRE || you.species == SP_KOBOLD)
        {
           you.mutation[MUT_SAPROVOROUS] =
           you.innate_mutation[MUT_SAPROVOROUS] = 0;
        }
        if (you.species == SP_OGRE)
        {
            // Remove the innate level of fast metabolism
            you.mutation[MUT_FAST_METABOLISM] -= 1;
            you.innate_mutation[MUT_FAST_METABOLISM] -= 1;
        }
    }

    if (th.getMinorVersion() < TAG_MINOR_CE_HA_DIET)
    {
        if (you.species == SP_CENTAUR)
        {
            you.mutation[MUT_FAST_METABOLISM] -= 1;
            you.innate_mutation[MUT_FAST_METABOLISM] -= 1;

            you.mutation[MUT_HERBIVOROUS] -= 1;
            you.innate_mutation[MUT_HERBIVOROUS] -= 1;
        }
        else if (you.species == SP_HALFLING)
        {
            you.mutation[MUT_SLOW_METABOLISM] -= 1;
            you.innate_mutation[MUT_SLOW_METABOLISM] -= 1;
        }
    }

    if (th.getMinorVersion() < TAG_MINOR_ROT_IMMUNITY)
    {
        if (you.species == SP_VINE_STALKER)
        {
            you.mutation[MUT_NO_DEVICE_HEAL] =
            you.innate_mutation[MUT_NO_DEVICE_HEAL] = 3;
        }

        if (you.species == SP_VINE_STALKER
            || you.species == SP_GARGOYLE)
        {
            you.mutation[MUT_ROT_IMMUNITY] =
            you.innate_mutation[MUT_ROT_IMMUNITY] = 1;
        }
    }

    if (th.getMinorVersion() < TAG_MINOR_FOUL_STENCH
        && you.species == SP_DEMONSPAWN
        && you.innate_mutation[MUT_SAPROVOROUS])
    {
        you.mutation[MUT_SAPROVOROUS] =
        you.innate_mutation[MUT_SAPROVOROUS] = 0;

        you.mutation[MUT_ROT_IMMUNITY] =
        you.innate_mutation[MUT_ROT_IMMUNITY] = 1;
    }

    if (th.getMinorVersion() < TAG_MINOR_DS_CLOUD_MUTATIONS
        && you.species == SP_DEMONSPAWN)
    {
        if (you.innate_mutation[MUT_CONSERVE_POTIONS])
        {
            you.mutation[MUT_CONSERVE_POTIONS] =
            you.innate_mutation[MUT_CONSERVE_POTIONS] = 0;

            you.mutation[MUT_FREEZING_CLOUD_IMMUNITY] =
            you.innate_mutation[MUT_FREEZING_CLOUD_IMMUNITY] = 1;
        }
        if (you.innate_mutation[MUT_CONSERVE_SCROLLS])
        {
            you.mutation[MUT_CONSERVE_SCROLLS] =
            you.innate_mutation[MUT_CONSERVE_SCROLLS] = 0;

            you.mutation[MUT_FLAME_CLOUD_IMMUNITY] =
            you.innate_mutation[MUT_FLAME_CLOUD_IMMUNITY] = 1;
        }
    }

    if (th.getMinorVersion() < TAG_MINOR_METABOLISM)
    {
        you.mutation[MUT_FAST_METABOLISM] =
        you.innate_mutation[MUT_FAST_METABOLISM];

        you.mutation[MUT_SLOW_METABOLISM] =
        you.innate_mutation[MUT_SLOW_METABOLISM];
    }
#endif

    count = unmarshallUByte(th);
    you.demonic_traits.clear();
    for (j = 0; j < count; ++j)
    {
        player::demon_trait dt;
        dt.level_gained = unmarshallByte(th);
        ASSERT_RANGE(dt.level_gained, 1, 28);
        dt.mutation = static_cast<mutation_type>(unmarshallShort(th));
#if TAG_MAJOR_VERSION == 34
        if (dt.mutation == MUT_CONSERVE_POTIONS)
            dt.mutation = MUT_FREEZING_CLOUD_IMMUNITY;
        else if (dt.mutation == MUT_CONSERVE_SCROLLS)
            dt.mutation = MUT_FLAME_CLOUD_IMMUNITY;
#endif
        ASSERT_RANGE(dt.mutation, 0, NUM_MUTATIONS);
        you.demonic_traits.push_back(dt);
    }

    EAT_CANARY;

    // how many penances?
    count = unmarshallUByte(th);
    ASSERT(count <= NUM_GODS);
    for (i = 0; i < count; i++)
    {
        you.penance[i] = unmarshallUByte(th);
        ASSERT(you.penance[i] <= MAX_PENANCE);
    }

#if TAG_MAJOR_VERSION == 34
    // Fix invalid ATTR_GOD_WRATH_XP if no god is giving penance.
    // cf. 0.14-a0-2640-g5c5a558
    if (you.attribute[ATTR_GOD_WRATH_XP] != 0
        || you.attribute[ATTR_GOD_WRATH_COUNT] != 0)
    {
        for (i = GOD_NO_GOD; i < NUM_GODS; ++i)
        {
            if (player_under_penance((god_type) i))
                break;
        }
        if (i == NUM_GODS)
        {
            you.attribute[ATTR_GOD_WRATH_XP] = 0;
            you.attribute[ATTR_GOD_WRATH_COUNT] = 0;
        }
    }
#endif

    for (i = 0; i < count; i++)
        you.worshipped[i] = unmarshallByte(th);

    for (i = 0; i < count; i++)
        you.num_current_gifts[i] = unmarshallShort(th);
    for (i = 0; i < count; i++)
        you.num_total_gifts[i] = unmarshallShort(th);
    for (i = 0; i < count; i++)
        you.one_time_ability_used.set(i, unmarshallBoolean(th));
    for (i = 0; i < count; i++)
        you.piety_max[i] = unmarshallByte(th);
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_NEMELEX_DUNGEONS)
    {
        unmarshallByte(th);
        for (i = 0; i < NEM_GIFT_SUMMONING; i++)
            unmarshallBoolean(th);
        unmarshallBoolean(th); // dungeons weight
        for (i = NEM_GIFT_SUMMONING; i < NUM_NEMELEX_GIFT_TYPES; i++)
            unmarshallBoolean(th);
    }
    else if (th.getMinorVersion() < TAG_MINOR_NEMELEX_WEIGHTS)
    {
        count = unmarshallByte(th);
        ASSERT(count == NUM_NEMELEX_GIFT_TYPES);
        for (i = 0; i < count; i++)
            unmarshallBoolean(th);
    }
#endif

    you.gift_timeout   = unmarshallByte(th);

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_BRANCH_ENTRY)
    {
        int depth = unmarshallByte(th);
        branch_type br = static_cast<branch_type>(unmarshallByte(th));
        ASSERT(br < NUM_BRANCHES);
        brentry[BRANCH_VESTIBULE] = level_id(br, depth);
    }
#endif

    you.exp_docked       = unmarshallInt(th);
    you.exp_docked_total = unmarshallInt(th);

    // elapsed time
    you.elapsed_time   = unmarshallInt(th);
    you.elapsed_time_at_last_input = you.elapsed_time;

    // Initialize new timers now that we know the time.
    const int last_20_turns = you.elapsed_time - (you.elapsed_time % 200);
    for (j = timer_count; j < NUM_TIMERS; ++j)
    {
        you.last_timer_effect[j] = last_20_turns;
        you.next_timer_effect[j] = last_20_turns + 200;
    }

    // Verify that timers aren't scheduled for the past.
    for (j = 0; j < NUM_TIMERS; ++j)
    {
        if (you.next_timer_effect[j] < you.elapsed_time)
        {
#if TAG_MAJOR_VERSION == 34
            if (th.getMinorVersion() >= TAG_MINOR_EVENT_TIMERS
                && th.getMinorVersion() < TAG_MINOR_EVENT_TIMER_FIX)
            {
                dprf("Fixing up timer %d from %d to %d",
                     j, you.next_timer_effect[j], last_20_turns + 200);
                you.last_timer_effect[j] = last_20_turns;
                you.next_timer_effect[j] = last_20_turns + 200;
            }
            else
#endif
            die("Timer %d next trigger in the past [%d < %d]",
                j, you.next_timer_effect[j], you.elapsed_time);
        }
    }

    // time of character creation
    you.birth_time = unmarshallInt(th);

    you.real_time  = unmarshallInt(th);
    you.num_turns  = unmarshallInt(th);
    you.exploration = unmarshallInt(th);

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_CONTAM_SCALE)
        you.magic_contamination = unmarshallShort(th) * 1000;
    else
#endif
        you.magic_contamination = unmarshallInt(th);

#if TAG_MAJOR_VERSION == 34
    unmarshallUByte(th);
#endif
    you.transit_stair  = unmarshallFeatureType(th);
    you.entering_level = unmarshallByte(th);
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() >= TAG_MINOR_TRAVEL_ALLY_PACE)
    {
#endif
        you.travel_ally_pace = unmarshallBoolean(th);
#if TAG_MAJOR_VERSION == 34
    }
#endif

    you.deaths = unmarshallByte(th);
    you.lives = unmarshallByte(th);

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() >= TAG_MINOR_LORC_TEMPERATURE)
    {
        you.temperature = unmarshallFloat(th);
        you.temperature_last = unmarshallFloat(th);
    }
    else
    {
        you.temperature = 0.0;
        you.temperature_last = 0.0;
    }
#endif

    you.dead = !you.hp;

    EAT_CANARY;

    int n_dact = unmarshallInt(th);
    ASSERT_RANGE(n_dact, 0, 100000); // arbitrary, sanity check
    you.dactions.resize(n_dact, NUM_DACTIONS);
    for (i = 0; i < n_dact; i++)
    {
        int a = unmarshallUByte(th);
        ASSERT(a < NUM_DACTIONS);
        you.dactions[i] = static_cast<daction_type>(a);
    }

    you.level_stack.clear();
    int n_levs = unmarshallInt(th);
    for (int k = 0; k < n_levs; k++)
    {
        level_pos pos;
        pos.load(th);
        you.level_stack.push_back(pos);
    }

    // List of currently beholding monsters (usually empty).
    count = unmarshallShort(th);
    ASSERT(count >= 0);
    for (i = 0; i < count; i++)
        you.beholders.push_back(unmarshallShort(th));

    // Also usually empty.
    count = unmarshallShort(th);
    ASSERT(count >= 0);
    for (i = 0; i < count; i++)
        you.fearmongers.push_back(unmarshallShort(th));

    you.piety_hysteresis = unmarshallByte(th);

    you.m_quiver->load(th);

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_FRIENDLY_PICKUP)
        unmarshallByte(th);
#endif

    you.zotdef_wave_name = unmarshallString(th);

    EAT_CANARY;

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() == TAG_MINOR_0_11)
    {
        for (unsigned int k = 0; k < 5; k++)
            unmarshallInt(th);
    }
#endif

    // Counts of actions made, by type.
    count = unmarshallShort(th);
    for (i = 0; i < count; i++)
    {
        caction_type caction = (caction_type)unmarshallShort(th);
        int subtype = unmarshallInt(th);
#if TAG_MAJOR_VERSION == 34
        if ((th.getMinorVersion() < TAG_MINOR_ACTION_THROW
             || th.getMinorVersion() == TAG_MINOR_0_11) && caction == CACT_THROW)
        {
            subtype = subtype | (OBJ_MISSILES << 16);
        }
#endif
        for (j = 0; j < 27; j++)
            you.action_count[pair<caction_type, int>(caction, subtype)][j] = unmarshallInt(th);
    }

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() >= TAG_MINOR_BRANCHES_LEFT) // 33:17 has it
    {
#endif
    count = unmarshallByte(th);
    for (i = 0; i < count; i++)
        you.branches_left.set(i, unmarshallBoolean(th));
#if TAG_MAJOR_VERSION == 34
    }
    else
    {
        // Assume all branches already exited in transferred games.
        you.branches_left.init(true);
    }
#endif

    abyssal_state.major_coord = unmarshallCoord(th);
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() >= TAG_MINOR_DEEP_ABYSS
        && th.getMinorVersion() != TAG_MINOR_0_11)
    {
        if (th.getMinorVersion() >= TAG_MINOR_REMOVE_ABYSS_SEED
            && th.getMinorVersion() < TAG_MINOR_ADD_ABYSS_SEED)
        {
            abyssal_state.seed = random_int();
        }
        else
#endif
            abyssal_state.seed = unmarshallInt(th);
        abyssal_state.depth = unmarshallInt(th);
        abyssal_state.destroy_all_terrain = false;
#if TAG_MAJOR_VERSION == 34
    }
    else
    {
        unmarshallFloat(th); // converted abyssal_state.depth to int.
        abyssal_state.depth = 0;
        abyssal_state.destroy_all_terrain = true;
        abyssal_state.seed = random_int();
    }
#endif
    abyssal_state.phase = unmarshallFloat(th);

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() >= TAG_MINOR_ABYSS_BRANCHES)
        abyssal_state.level = unmarshall_level_id(th);
    if (!abyssal_state.level.is_valid())
    {
        abyssal_state.level.branch = BRANCH_DEPTHS;
        abyssal_state.level.depth = 1;
    }
#else
    abyssal_state.level = unmarshall_level_id(th);
#endif

    _unmarshall_constriction(th, &you);

    you.octopus_king_rings = unmarshallUByte(th);

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() >= TAG_MINOR_UNCANCELLABLES
        && th.getMinorVersion() != TAG_MINOR_0_11)
    {
#endif
    count = unmarshallUnsigned(th);
    ASSERT_RANGE(count, 0, 16); // sanity check
    you.uncancel.resize(count);
    for (i = 0; i < count; i++)
    {
        you.uncancel[i].first = (uncancellable_type)unmarshallUByte(th);
        you.uncancel[i].second = unmarshallInt(th);
    }
#if TAG_MAJOR_VERSION == 34
    }

    if (th.getMinorVersion() >= TAG_MINOR_INCREMENTAL_RECALL)
    {
#endif
    count = unmarshallUnsigned(th);
    you.recall_list.resize(count);
    for (i = 0; i < count; i++)
        you.recall_list[i] = unmarshall_int_as<mid_t>(th);
#if TAG_MAJOR_VERSION == 34
    }

    if (th.getMinorVersion() >= TAG_MINOR_SEEDS)
    {
#endif
    count = unmarshallUByte(th);
    ASSERT(count <= NUM_SEEDS);
    for (i = 0; i < count; i++)
        you.game_seeds[i] = unmarshallInt(th);
    for (i = count; i < NUM_SEEDS; i++)
        you.game_seeds[i] = random_int();
#if TAG_MAJOR_VERSION == 34
    }
#endif

    EAT_CANARY;

    if (!dlua.callfn("dgn_load_data", "u", &th))
    {
        mprf(MSGCH_ERROR, "Failed to load Lua persist table: %s",
             dlua.error.c_str());
    }

    EAT_CANARY;

    unmarshallString(th);

    you.props.clear();
    you.props.read(th);
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_STICKY_FLAME)
    {
        if (you.props.exists("napalmer"))
            you.props["sticky_flame_source"] = you.props["napalmer"];
        if (you.props.exists("napalm_aux"))
            you.props["sticky_flame_aux"] = you.props["napalm_aux"];
    }
    if (you.duration[DUR_WEAPON_BRAND] && !you.props.exists(ORIGINAL_BRAND_KEY))
        you.props[ORIGINAL_BRAND_KEY] = SPWPN_NORMAL;
#endif
}

static void tag_read_you_items(reader &th)
{
    int i,j;
    int count, count2;

    // how many inventory slots?
    count = unmarshallByte(th);
    ASSERT(count == ENDOFPACK); // not supposed to change
    for (i = 0; i < count; ++i)
        unmarshallItem(th, you.inv[i]);

    // Initialize cache of equipped unrand functions
    for (i = 0; i < NUM_EQUIP; ++i)
    {
        const item_def *item = you.slot_item(static_cast<equipment_type>(i));

        if (item && is_unrandom_artefact(*item))
        {
            const unrandart_entry *entry = get_unrand_entry(item->special);

            if (entry->world_reacts_func)
                you.unrand_reacts.set(i);
        }
    }

    unmarshallFixedBitVector<NUM_RUNE_TYPES>(th, you.runes);
    you.obtainable_runes = unmarshallByte(th);

    // Item descrip for each type & subtype.
    // how many types?
    count = unmarshallUByte(th);
    ASSERT(count <= NUM_IDESC);
    // how many subtypes?
    count2 = unmarshallUByte(th);
    ASSERT(count2 <= MAX_SUBTYPES);
    for (i = 0; i < count; ++i)
        for (j = 0; j < count2; ++j)
            you.item_description[i][j] = unmarshallByte(th);
    for (i = 0; i < count; ++i)
        for (j = count2; j < MAX_SUBTYPES; ++j)
            you.item_description[i][j] = 0;
    int iclasses = unmarshallUByte(th);
    ASSERT(iclasses <= NUM_OBJECT_CLASSES);

    // Identification status.
    for (i = 0; i < iclasses; ++i)
    {
        if (!item_type_has_ids((object_class_type)i))
            continue;
        for (j = 0; j < count2; ++j)
        {
#if TAG_MAJOR_VERSION == 34
            uint8_t x;

            if (th.getMinorVersion() < TAG_MINOR_BOOK_ID && i == OBJ_BOOKS)
                x = ID_UNKNOWN_TYPE;
            else
                x = unmarshallUByte(th);

            ASSERT(x < NUM_ID_STATE_TYPES);
            you.type_ids[i][j] = static_cast<item_type_id_state_type>(x);
#else
            you.type_ids[i][j] = static_cast<item_type_id_state_type>(
                                     unmarshallUByte(th));
#endif
        }
        for (j = count2; j < MAX_SUBTYPES; ++j)
            you.type_ids[i][j] = ID_UNKNOWN_TYPE;
    }

    // Additional identification info
    you.type_id_props.read(th);

    EAT_CANARY;

    // how many unique items?
    count = unmarshallUByte(th);
    COMPILE_CHECK(NO_UNRANDARTS <= 256);
    for (j = 0; j < count && j < NO_UNRANDARTS; ++j)
    {
        you.unique_items[j] =
            static_cast<unique_item_status_type>(unmarshallByte(th));
    }
    // # of unrandarts could certainly change.
    // If it does, the new ones won't exist yet - zero them out.
    for (; j < NO_UNRANDARTS; j++)
        you.unique_items[j] = UNIQ_NOT_EXISTS;
    for (j = NO_UNRANDARTS; j < count; j++)
        unmarshallByte(th);

    // how many books?
    count = unmarshallUByte(th);
    COMPILE_CHECK(NUM_FIXED_BOOKS <= 256);
    for (j = 0; j < count && j < NUM_FIXED_BOOKS; ++j)
        you.had_book.set(j, unmarshallByte(th));
    for (j = count; j < NUM_FIXED_BOOKS; ++j)
        you.had_book.set(j, false);
    for (j = NUM_FIXED_BOOKS; j < count; ++j)
        unmarshallByte(th);

    // how many spells?
    count = unmarshallShort(th);
    ASSERT(count >= 0);
    for (j = 0; j < count && j < NUM_SPELLS; ++j)
        you.seen_spell.set(j, unmarshallByte(th));
    for (j = count; j < NUM_SPELLS; ++j)
        you.seen_spell.set(j, false);
    for (j = NUM_SPELLS; j < count; ++j)
        unmarshallByte(th);

    count = unmarshallShort(th);
    ASSERT(count >= 0);
    for (j = 0; j < count && j < NUM_WEAPONS; ++j)
        you.seen_weapon[j] = unmarshallInt(th);
    for (j = count; j < NUM_WEAPONS; ++j)
        you.seen_weapon[j] = 0;
    for (j = NUM_WEAPONS; j < count; ++j)
        unmarshallInt(th);

    count = unmarshallShort(th);
    ASSERT(count >= 0);
    for (j = 0; j < count && j < NUM_ARMOURS; ++j)
        you.seen_armour[j] = unmarshallInt(th);
    for (j = count; j < NUM_ARMOURS; ++j)
        you.seen_armour[j] = 0;
    for (j = NUM_ARMOURS; j < count; ++j)
        unmarshallInt(th);
    unmarshallFixedBitVector<NUM_MISCELLANY>(th, you.seen_misc);

    for (i = 0; i < iclasses; i++)
        for (j = 0; j < count2; j++)
            you.force_autopickup[i][j] = unmarshallInt(th);
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_FOOD_AUTOPICKUP)
    {
        const int oldstate = you.force_autopickup[OBJ_FOOD][NUM_FOODS];
        you.force_autopickup[OBJ_FOOD][FOOD_MEAT_RATION] = oldstate;
        you.force_autopickup[OBJ_FOOD][FOOD_FRUIT] = oldstate;
        you.force_autopickup[OBJ_FOOD][FOOD_ROYAL_JELLY] = oldstate;

        you.force_autopickup[OBJ_BOOKS][BOOK_MANUAL] =
            you.force_autopickup[OBJ_BOOKS][NUM_BOOKS];
    }
    if (th.getMinorVersion() < TAG_MINOR_FOOD_PURGE_AP_FIX)
    {
        FixedVector<int, MAX_SUBTYPES> &food_pickups =
            you.force_autopickup[OBJ_FOOD];

        // If fruit pickup was not set explicitly during the time between
        // FOOD_PURGE and FOOD_PURGE_AP_FIX, copy the old exemplar FOOD_PEAR.
        if (food_pickups[FOOD_FRUIT] == 0)
            food_pickups[FOOD_FRUIT] = food_pickups[FOOD_PEAR];
    }
    if (you.species == SP_FORMICID)
        remove_one_equip(EQ_HELMET, false, true);
#endif
}

static PlaceInfo unmarshallPlaceInfo(reader &th)
{
    PlaceInfo place_info;

#if TAG_MAJOR_VERSION == 34
    int br = unmarshallInt(th);
    if (br == -1)
        br = NUM_BRANCHES;
    ASSERT(br >= 0);
    place_info.branch      = static_cast<branch_type>(br);
#else
    place_info.branch      = static_cast<branch_type>(unmarshallInt(th));
#endif

    place_info.num_visits  = unmarshallInt(th);
    place_info.levels_seen = unmarshallInt(th);

    place_info.mon_kill_exp       = unmarshallInt(th);

    for (int i = 0; i < KC_NCATEGORIES; i++)
        place_info.mon_kill_num[i] = unmarshallInt(th);

    place_info.turns_total      = unmarshallInt(th);
    place_info.turns_explore    = unmarshallInt(th);
    place_info.turns_travel     = unmarshallInt(th);
    place_info.turns_interlevel = unmarshallInt(th);
    place_info.turns_resting    = unmarshallInt(th);
    place_info.turns_other      = unmarshallInt(th);

    place_info.elapsed_total      = unmarshallInt(th);
    place_info.elapsed_explore    = unmarshallInt(th);
    place_info.elapsed_travel     = unmarshallInt(th);
    place_info.elapsed_interlevel = unmarshallInt(th);
    place_info.elapsed_resting    = unmarshallInt(th);
    place_info.elapsed_other      = unmarshallInt(th);

    return place_info;
}

#if TAG_MAJOR_VERSION == 34
static branch_type old_entries[] =
{
    /* D */      NUM_BRANCHES,
    /* Temple */ BRANCH_DUNGEON,
    /* Orc */    BRANCH_DUNGEON,
    /* Elf */    BRANCH_ORC,
    /* Dwarf */  BRANCH_ELF,
    /* Lair */   BRANCH_DUNGEON,
    /* Swamp */  BRANCH_LAIR,
    /* Shoals */ BRANCH_LAIR,
    /* Snake */  BRANCH_LAIR,
    /* Spider */ BRANCH_LAIR,
    /* Slime */  BRANCH_LAIR,
    /* Vaults */ BRANCH_DUNGEON,
    /* Blade */  BRANCH_VAULTS,
    /* Crypt */  BRANCH_VAULTS,
    /* Tomb */   BRANCH_CRYPT, // or Forest
    /* Hell */   NUM_BRANCHES,
    /* Dis */    BRANCH_VESTIBULE,
    /* Geh */    BRANCH_VESTIBULE,
    /* Coc */    BRANCH_VESTIBULE,
    /* Tar */    BRANCH_VESTIBULE,
    /* Zot */    BRANCH_DUNGEON,
    /* Forest */ BRANCH_VAULTS,
    /* Abyss */  NUM_BRANCHES,
    /* Pan */    NUM_BRANCHES,
    /* various portal branches */ NUM_BRANCHES,
    NUM_BRANCHES, NUM_BRANCHES, NUM_BRANCHES, NUM_BRANCHES, NUM_BRANCHES,
    NUM_BRANCHES, NUM_BRANCHES, NUM_BRANCHES, NUM_BRANCHES, NUM_BRANCHES,
    NUM_BRANCHES,
};
#endif

static void tag_read_you_dungeon(reader &th)
{
    // how many unique creatures?
    int count = unmarshallShort(th);
    you.unique_creatures.reset();
    for (int j = 0; j < count; ++j)
    {
        const bool created = unmarshallBoolean(th);

        if (j < NUM_MONSTERS)
            you.unique_creatures.set(j, created);
    }

    // how many branches?
    count = unmarshallUByte(th);
    ASSERT(count <= NUM_BRANCHES);
    for (int j = 0; j < count; ++j)
    {
        brdepth[j]    = unmarshallInt(th);
        ASSERT_RANGE(brdepth[j], -1, MAX_BRANCH_DEPTH + 1);
#if TAG_MAJOR_VERSION == 34
        if (th.getMinorVersion() < TAG_MINOR_BRANCH_ENTRY)
        {
            int depth = unmarshallInt(th);
            if (j != BRANCH_VESTIBULE)
                brentry[j] = level_id(old_entries[j], depth);
        }
        else
#endif
        brentry[j]    = unmarshall_level_id(th);
#if TAG_MAJOR_VERSION == 34
        if (th.getMinorVersion() < TAG_MINOR_BRIBE_BRANCH)
            branch_bribe[j] = 0;
        else
#endif
        branch_bribe[j] = unmarshallInt(th);
    }
#if TAG_MAJOR_VERSION == 34
    // Deepen the Abyss; this is okay since new abyssal stairs will be
    // generated as the place shifts.
    if (crawl_state.game_is_normal() && th.getMinorVersion() <= TAG_MINOR_ORIG_MONNUM)
        brdepth[BRANCH_ABYSS] = 5;
#endif

    ASSERT(you.depth <= brdepth[you.where_are_you]);

    // Root of the dungeon; usually BRANCH_DUNGEON.
    root_branch = static_cast<branch_type>(unmarshallInt(th));
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_BRANCH_ENTRY)
    {
        brentry[root_branch].clear();
        if (brentry[BRANCH_FOREST].is_valid())
            brentry[BRANCH_TOMB].branch = BRANCH_FOREST;
    }
#endif

    unmarshallMap(th, stair_level,
                  unmarshall_int_as<branch_type>,
                  _unmarshall_level_id_set);
    unmarshallMap(th, shops_present,
                  _unmarshall_level_pos, unmarshall_int_as<shop_type>);
    unmarshallMap(th, altars_present,
                  _unmarshall_level_pos, unmarshall_int_as<god_type>);
    unmarshallMap(th, portals_present,
                  _unmarshall_level_pos, unmarshall_int_as<branch_type>);
    unmarshallMap(th, portal_notes,
                  _unmarshall_level_pos, unmarshallString);
    unmarshallMap(th, level_annotations,
                  unmarshall_level_id, unmarshallString);
    unmarshallMap(th, level_exclusions,
                  unmarshall_level_id, unmarshallString);
    unmarshallMap(th, level_uniques,
                  unmarshall_level_id, unmarshallString);
    unmarshallUniqueAnnotations(th);

    PlaceInfo place_info = unmarshallPlaceInfo(th);
    ASSERT(place_info.is_global());
    you.set_place_info(place_info);

    vector<PlaceInfo> list = you.get_all_place_info();
    unsigned short count_p = (unsigned short) unmarshallShort(th);
    // Use "<=" so that adding more branches or non-dungeon places
    // won't break save-file compatibility.
    ASSERT(count_p <= list.size());

    for (int i = 0; i < count_p; i++)
    {
        place_info = unmarshallPlaceInfo(th);
        ASSERT(!place_info.is_global());
        you.set_place_info(place_info);
    }

    typedef pair<string_set::iterator, bool> ssipair;
    unmarshall_container(th, you.uniq_map_tags,
                         (ssipair (string_set::*)(const string &))
                         &string_set::insert,
                         unmarshallString);
    unmarshall_container(th, you.uniq_map_names,
                         (ssipair (string_set::*)(const string &))
                         &string_set::insert,
                         unmarshallString);
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() >= TAG_MINOR_VAULT_LIST) // 33:17 has it
#endif
    unmarshallMap(th, you.vault_list, unmarshall_level_id,
                  unmarshallStringVector);

    read_level_connectivity(th);
}

static void tag_read_lost_monsters(reader &th)
{
    the_lost_ones.clear();
    unmarshallMap(th, the_lost_ones,
                  unmarshall_level_id, unmarshall_follower_list);
}

static void tag_read_lost_items(reader &th)
{
    transiting_items.clear();

    unmarshallMap(th, transiting_items,
                  unmarshall_level_id, unmarshall_item_list);
}

static void tag_read_companions(reader &th)
{
    companion_list.clear();

    unmarshallMap(th, companion_list, unmarshall_int_as<mid_t>,
                  unmarshall_companion);
}

template <typename Z>
static int _last_used_index(const Z &thinglist, int max_things)
{
    for (int i = max_things - 1; i >= 0; --i)
        if (thinglist[i].defined())
            return i + 1;
    return 0;
}

// ------------------------------- level tags ---------------------------- //

static void tag_construct_level(writer &th)
{
    marshallByte(th, env.floor_colour);
    marshallByte(th, env.rock_colour);

    marshallInt(th, env.level_flags);

    marshallInt(th, you.on_current_level ? you.elapsed_time : env.elapsed_time);
    marshallCoord(th, you.pos());

    // Map grids.
    // how many X?
    marshallShort(th, GXM);
    // how many Y?
    marshallShort(th, GYM);

    marshallInt(th, env.turns_on_level);

    CANARY;

    for (int count_x = 0; count_x < GXM; count_x++)
        for (int count_y = 0; count_y < GYM; count_y++)
        {
            marshallByte(th, grd[count_x][count_y]);
            marshallMapCell(th, env.map_knowledge[count_x][count_y]);
            marshallInt(th, env.pgrid[count_x][count_y]);
        }

    marshallBoolean(th, !!env.map_forgotten.get());
    if (env.map_forgotten.get())
        for (int x = 0; x < GXM; x++)
            for (int y = 0; y < GYM; y++)
                marshallMapCell(th, (*env.map_forgotten)[x][y]);

    _run_length_encode(th, marshallByte, env.grid_colours, GXM, GYM);

    CANARY;

    // how many clouds?
    const int nc = _last_used_index(env.cloud, MAX_CLOUDS);
    marshallShort(th, nc);
    for (int i = 0; i < nc; i++)
    {
        marshallByte(th, env.cloud[i].type);
        if (env.cloud[i].type == CLOUD_NONE)
            continue;
        ASSERT_IN_BOUNDS(env.cloud[i].pos);
        marshallByte(th, env.cloud[i].pos.x);
        marshallByte(th, env.cloud[i].pos.y);
        marshallShort(th, env.cloud[i].decay);
        marshallByte(th, env.cloud[i].spread_rate);
        marshallByte(th, env.cloud[i].whose);
        marshallByte(th, env.cloud[i].killer);
        marshallInt(th, env.cloud[i].source);
        marshallShort(th, env.cloud[i].colour);
        marshallString(th, env.cloud[i].name);
        marshallString(th, env.cloud[i].tile);
        marshallInt(th, env.cloud[i].excl_rad);
    }

    CANARY;

    // how many shops?
    const int ns = _last_used_index(env.shop, MAX_SHOPS);
    marshallShort(th, ns);
    for (int i = 0; i < ns; i++)
    {
        marshallByte(th, env.shop[i].type);
        if (env.shop[i].type == SHOP_UNASSIGNED)
            continue;
        marshallByte(th, env.shop[i].keeper_name[0]);
        marshallByte(th, env.shop[i].keeper_name[1]);
        marshallByte(th, env.shop[i].keeper_name[2]);
        marshallByte(th, env.shop[i].pos.x);
        marshallByte(th, env.shop[i].pos.y);
        marshallByte(th, env.shop[i].greed);
        marshallByte(th, env.shop[i].level);
        marshallString(th, env.shop[i].shop_name);
        marshallString(th, env.shop[i].shop_type_name);
        marshallString(th, env.shop[i].shop_suffix_name);
    }

    CANARY;

    marshallCoord(th, env.sanctuary_pos);
    marshallByte(th, env.sanctuary_time);

    marshallInt(th, env.spawn_random_rate);

    env.markers.write(th);
    env.properties.write(th);

    marshallInt(th, you.dactions.size());

    // Save heightmap, if present.
    marshallByte(th, !!env.heightmap.get());
    if (env.heightmap.get())
    {
        grid_heightmap &heightmap(*env.heightmap);
        for (rectangle_iterator ri(0); ri; ++ri)
            marshallShort(th, heightmap(*ri));
    }

    CANARY;

    marshallInt(th, env.forest_awoken_until);
    marshall_level_vault_data(th);
    marshallInt(th, env.density);

    marshallShort(th, env.sunlight.size());
    for (size_t i = 0; i < env.sunlight.size(); ++i)
    {
        marshallCoord(th, env.sunlight[i].first);
        marshallInt(th, env.sunlight[i].second);
    }
}

void marshallItem(writer &th, const item_def &item, bool iinfo)
{
    marshallByte(th, item.base_type);
    if (item.base_type == OBJ_UNASSIGNED)
        return;

#if TAG_MAJOR_VERSION == 34
    if (!item.is_valid(iinfo))
    {
        string name;
        item_def dummy = item;
        if (!item.quantity)
            name = "(quantity: 0) ", dummy.quantity = 1;
        name += dummy.name(DESC_PLAIN, true);
        die("Invalid item: %s", name.c_str());
    }
#endif
    ASSERT(item.is_valid(iinfo));

    marshallByte(th, item.sub_type);
    marshallShort(th, item.plus);
    marshallShort(th, item.plus2);
    marshallInt(th, item.special);
    marshallShort(th, item.quantity);

    marshallByte(th, item.colour);
    marshallByte(th, item.rnd);
    marshallShort(th, item.pos.x);
    marshallShort(th, item.pos.y);
    marshallInt(th, item.flags);

    marshallShort(th, item.link);
    if (item.pos.x >= 0 && item.pos.y >= 0)
        marshallShort(th, igrd(item.pos));  //  unused
    else
        marshallShort(th, -1); // unused

    marshallByte(th, item.slot);

    marshallShort(th, item.orig_place);
    marshallShort(th, item.orig_monnum);
    marshallString(th, item.inscription.c_str());

    item.props.write(th);
}

#if TAG_MAJOR_VERSION == 34
static void _trim_god_gift_inscrip(item_def& item)
{
    item.inscription = replace_all(item.inscription, "god gift, ", "");
    item.inscription = replace_all(item.inscription, "god gift", "");
    item.inscription = replace_all(item.inscription, "Psyche", "");
    item.inscription = replace_all(item.inscription, "Sonja", "");
    item.inscription = replace_all(item.inscription, "Donald", "");
}
#endif

void unmarshallItem(reader &th, item_def &item)
{
    item.base_type   = static_cast<object_class_type>(unmarshallByte(th));
    if (item.base_type == OBJ_UNASSIGNED)
        return;
    item.sub_type    = unmarshallUByte(th);
    item.plus        = unmarshallShort(th);
    item.plus2       = unmarshallShort(th);
    item.special     = unmarshallInt(th);
    item.quantity    = unmarshallShort(th);
    item.colour      = unmarshallUByte(th);
    item.rnd         = unmarshallUByte(th);

    item.pos.x       = unmarshallShort(th);
    item.pos.y       = unmarshallShort(th);
    item.flags       = unmarshallInt(th);
    item.link        = unmarshallShort(th);

    unmarshallShort(th);  // igrd[item.x][item.y] -- unused

    item.slot        = unmarshallByte(th);

    item.orig_place  = unmarshallShort(th);
    item.orig_monnum = unmarshallShort(th);
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_ORIG_MONNUM && item.orig_monnum > 0)
        item.orig_monnum--;
#endif
    item.inscription = unmarshallString(th);

    item.props.clear();
    item.props.read(th);

    // Fixup artefact props to handle reloading items when the new version
    // of Crawl has more artefact props.
    if (is_artefact(item))
        artefact_fixup_props(item);

#if TAG_MAJOR_VERSION == 34
    // Blowguns aren't supposed to have to-dam since 0.7, but they were
    // generated with a plus2 anyway.
    if (item.base_type == OBJ_WEAPONS && item.sub_type == WPN_BLOWGUN)
        item.plus2 = 0;

    // Remove artefact autoinscriptions from the saved inscription.
    if ((th.getMinorVersion() < TAG_MINOR_AUTOINSCRIPTIONS
         || th.getMinorVersion() == TAG_MINOR_0_11) && is_artefact(item))
    {
        string art_ins = artefact_inscription(item);
        if (!art_ins.empty())
        {
            item.inscription = replace_all(item.inscription, art_ins + ",", "");
            item.inscription = replace_all(item.inscription, art_ins, "");

            // Avoid q - the ring "Foo" {+Fly rF+, +Lev rF+}
            art_ins = replace_all(art_ins, "+Fly", "+Lev");
            item.inscription = replace_all(item.inscription, art_ins + ",", "");
            item.inscription = replace_all(item.inscription, art_ins, "");

            trim_string(item.inscription);
        }
    }

    // Upgrade item knowledge to cope with the fix for #1083
    if (item.base_type == OBJ_JEWELLERY)
    {
        if (item.flags & ISFLAG_KNOW_PROPERTIES)
            item.flags |= ISFLAG_KNOW_TYPE;
    }

    if (item.base_type == OBJ_POTIONS && item.sub_type == POT_WATER)
        item.sub_type = POT_CONFUSION;
    if (item.base_type == OBJ_STAVES && item.sub_type == STAFF_CHANNELING)
        item.sub_type = STAFF_ENERGY;

    if (th.getMinorVersion() < TAG_MINOR_GOD_GIFT)
    {
        _trim_god_gift_inscrip(item);
        if (is_stackable_item(item))
            origin_reset(item);
    }

    if (th.getMinorVersion() < TAG_MINOR_NO_SPLINT
        && item.base_type == OBJ_ARMOUR && item.sub_type > ARM_CHAIN_MAIL)
    {
        --item.sub_type;
    }

    if (th.getMinorVersion() < TAG_MINOR_BOX_OF_BEASTS_CHARGES
        && item.base_type == OBJ_MISCELLANY && item.sub_type == MISC_BOX_OF_BEASTS)
    {
        // Give charges to box of beasts. If the player used it
        // already then, well, they got some freebies.
        item.plus = random_range(5, 15, 2);
    }

    if (item.base_type == OBJ_MISCELLANY && item.sub_type == MISC_BUGGY_EBONY_CASKET)
    {
        item.sub_type = MISC_BOX_OF_BEASTS;
        item.plus = 1;
    }

    // was spiked flail; rods can't spawn
    if (item.base_type == OBJ_WEAPONS && item.sub_type == WPN_ROD
        && th.getMinorVersion() <= TAG_MINOR_FORGOTTEN_MAP)
    {
        item.sub_type = WPN_FLAIL;
    }

    if (item.base_type == OBJ_WEAPONS
        && (item.special == SPWPN_RETURNING
            || item.special == SPWPN_REACHING
            || item.special == SPWPN_ORC_SLAYING
            || item.special == SPWPN_DRAGON_SLAYING))
    {
        item.special = SPWPN_NORMAL;
    }

    // Not putting these in a minor tag since it's possible for an old
    // random monster spawn list to place flame/frost weapons.
    if (item.base_type == OBJ_WEAPONS && get_weapon_brand(item) == SPWPN_FROST)
    {
        if (is_artefact(item))
            artefact_set_property(item, ARTP_BRAND, SPWPN_FREEZING);
        else
            item.special = SPWPN_FREEZING;
    }
    if (item.base_type == OBJ_WEAPONS && get_weapon_brand(item) == SPWPN_FLAME)
    {
        if (is_artefact(item))
            artefact_set_property(item, ARTP_BRAND, SPWPN_FLAMING);
        else
            item.special = SPWPN_FLAMING;
    }

    if (item.base_type == OBJ_MISCELLANY && item.sub_type == MISC_HORN_OF_GERYON
        && th.getMinorVersion() < TAG_MINOR_HORN_GERYON_CHANGE)
    {
        item.plus2 = 0;
    }

    // Rescale old MR (range 35-99) to new discrete steps (40/80/120)
    // Negative MR was only supposed to exist for Folly, but paranoia.
    if (th.getMinorVersion() < TAG_MINOR_MR_ITEM_RESCALE
        && is_artefact(item)
        && artefact_wpn_property(item, ARTP_MAGIC))
    {
        int prop_mr = artefact_wpn_property(item, ARTP_MAGIC);
        if (prop_mr > 99)
            artefact_set_property(item, ARTP_MAGIC, 3);
        else if (prop_mr > 79)
            artefact_set_property(item, ARTP_MAGIC, 2);
        else if (prop_mr < -40)
            artefact_set_property(item, ARTP_MAGIC, -2);
        else if (prop_mr < 0)
            artefact_set_property(item, ARTP_MAGIC, -1);
        else
            artefact_set_property(item, ARTP_MAGIC, 1);
    }

    // Rescale stealth (range 10..79 and -10..-98) to discrete steps (+-50/100)
    if (th.getMinorVersion() < TAG_MINOR_STEALTH_RESCALE && is_artefact(item))
    {
        if (artefact_wpn_property(item, ARTP_STEALTH))
        {
            int prop_st = artefact_wpn_property(item, ARTP_STEALTH);
            if (prop_st > 60)
                artefact_set_property(item, ARTP_STEALTH, 2);
            else if (prop_st < -70)
                artefact_set_property(item, ARTP_STEALTH, -2);
            else if (prop_st < 0)
                artefact_set_property(item, ARTP_STEALTH, -1);
            else
                artefact_set_property(item, ARTP_STEALTH, 1);
        }

        // Remove fast metabolism property
        if (artefact_wpn_property(item, ARTP_METABOLISM))
        {
            artefact_set_property(item, ARTP_METABOLISM, 0);
            artefact_set_property(item, ARTP_STEALTH, -1);
        }

        // Make sure no weird fake-rap combinations are produced by the upgrade
        // from rings of sustenance/hunger with {Stlth} to stealth/loudness
        if (item.base_type == OBJ_JEWELLERY
            && (item.sub_type == RING_STEALTH || item.sub_type == RING_LOUDNESS))
        {
            artefact_set_property(item, ARTP_STEALTH, 0);
        }
    }

    if (th.getMinorVersion() < TAG_MINOR_NO_POT_FOOD)
    {
        // Replace War Chants with Battle to avoid empty-book errors.

        // Moved under TAG_MINOR_NO_POT_FOOD because it was formerly
        // not restricted to a particular range of minor tags.
        if (item.base_type == OBJ_BOOKS && item.sub_type == BOOK_WAR_CHANTS)
            item.sub_type = BOOK_BATTLE;

        if (item.base_type == OBJ_FOOD && (item.sub_type == FOOD_UNUSED
                                           || item.sub_type == FOOD_AMBROSIA))
        {
            item.sub_type = FOOD_ROYAL_JELLY;
        }
    }

    if (th.getMinorVersion() < TAG_MINOR_FOOD_PURGE)
    {
        if (item.base_type == OBJ_FOOD)
        {
            if (item.sub_type == FOOD_SAUSAGE)
                item.sub_type = FOOD_BEEF_JERKY;
            if (item.sub_type == FOOD_CHEESE)
                item.sub_type = FOOD_PIZZA;
            if (item.sub_type == FOOD_PEAR
                || item.sub_type == FOOD_APPLE
                || item.sub_type == FOOD_CHOKO
                || item.sub_type == FOOD_APRICOT
                || item.sub_type == FOOD_ORANGE
                || item.sub_type == FOOD_BANANA
                || item.sub_type == FOOD_STRAWBERRY
                || item.sub_type == FOOD_RAMBUTAN
                || item.sub_type == FOOD_LEMON
                || item.sub_type == FOOD_GRAPE
                || item.sub_type == FOOD_SULTANA
                || item.sub_type == FOOD_LYCHEE
                || item.sub_type == FOOD_LEMON)
            {
                item.sub_type = FOOD_FRUIT;
            }
        }
    }

    // Combine old rings of slaying (Acc/Dam) to new (Dam).
    // Also handle the changes to the respective ARTP_.
    if (th.getMinorVersion() < TAG_MINOR_SLAYRING_PLUSES)
    {
        int acc, dam, slay = 0;

        if (item.props.exists(ARTEFACT_PROPS_KEY))
        {
            acc = artefact_wpn_property(item, ARTP_ACCURACY);
            dam = artefact_wpn_property(item, ARTP_SLAYING);
            slay = dam < 0 ? dam : max(acc, dam);

            artefact_set_property(item, ARTP_SLAYING, slay);
        }

        if (item.base_type == OBJ_JEWELLERY && item.sub_type == RING_SLAYING)
        {
             acc = item.plus;
             dam = item.plus2;
             slay = dam < 0 ? dam : max(acc, dam);

            item.plus = slay;
            item.plus2 = 0;
        }
    }

    if (th.getMinorVersion() < TAG_MINOR_MERGE_EW)
    {
        // Combine EW1/EW2/EW3 scrolls into single enchant weapon scroll.
        if (item.base_type == OBJ_SCROLLS
            && (item.sub_type == SCR_ENCHANT_WEAPON_II
                || item.sub_type == SCR_ENCHANT_WEAPON_III))
        {
            item.sub_type = SCR_ENCHANT_WEAPON;
        }
    }

    if (th.getMinorVersion() < TAG_MINOR_WEAPON_PLUSES)
    {
        int acc, dam, slay = 0;

        if (item.base_type == OBJ_WEAPONS)
        {
            acc = item.plus;
            dam = item.plus2;
            slay = dam < 0 ? dam : max(acc,dam);

            item.plus = slay;
            item.plus2 = 0;
        }
    }
#endif

    if (is_unrandom_artefact(item))
        setup_unrandart(item);

    bind_item_tile(item);
}

#define MAP_SERIALIZE_FLAGS_MASK 3
#define MAP_SERIALIZE_FLAGS_8 1
#define MAP_SERIALIZE_FLAGS_16 2
#define MAP_SERIALIZE_FLAGS_32 3

#define MAP_SERIALIZE_FEATURE 4
#define MAP_SERIALIZE_FEATURE_COLOUR 8
#define MAP_SERIALIZE_ITEM 0x10
#define MAP_SERIALIZE_CLOUD 0x20
#define MAP_SERIALIZE_MONSTER 0x40

void marshallMapCell(writer &th, const map_cell &cell)
{
    unsigned flags = 0;

    if (cell.flags > 0xffff)
        flags |= MAP_SERIALIZE_FLAGS_32;
    else if (cell.flags > 0xff)
        flags |= MAP_SERIALIZE_FLAGS_16;
    else if (cell.flags)
        flags |= MAP_SERIALIZE_FLAGS_8;

    if (cell.feat() != DNGN_UNSEEN)
        flags |= MAP_SERIALIZE_FEATURE;

    if (cell.feat_colour())
        flags |= MAP_SERIALIZE_FEATURE_COLOUR;

    if (cell.cloud() != CLOUD_NONE)
        flags |= MAP_SERIALIZE_CLOUD;

    if (cell.item())
        flags |= MAP_SERIALIZE_ITEM;

    if (cell.monster() != MONS_NO_MONSTER)
        flags |= MAP_SERIALIZE_MONSTER;

    marshallUnsigned(th, flags);

    switch (flags & MAP_SERIALIZE_FLAGS_MASK)
    {
    case MAP_SERIALIZE_FLAGS_8:
        marshallByte(th, cell.flags);
        break;
    case MAP_SERIALIZE_FLAGS_16:
        marshallShort(th, cell.flags);
        break;
    case MAP_SERIALIZE_FLAGS_32:
        marshallInt(th, cell.flags);
        break;
    }

    if (flags & MAP_SERIALIZE_FEATURE)
#if TAG_MAJOR_VERSION == 34
        marshallUnsigned(th, cell.feat());
#else
        marshallUByte(th, cell.feat());
#endif

    if (flags & MAP_SERIALIZE_FEATURE_COLOUR)
        marshallUnsigned(th, cell.feat_colour());

    if (feat_is_trap(cell.feat()))
        marshallByte(th, cell.trap());

    if (flags & MAP_SERIALIZE_CLOUD)
    {
        cloud_info* ci = cell.cloudinfo();
        marshallUnsigned(th, ci->type);
        marshallUnsigned(th, ci->colour);
        marshallUnsigned(th, ci->duration);
        marshallShort(th, ci->tile);
        marshallUByte(th, ci->killer);
    }

    if (flags & MAP_SERIALIZE_ITEM)
        marshallItem(th, *cell.item(), true);

    if (flags & MAP_SERIALIZE_MONSTER)
        marshallMonsterInfo(th, *cell.monsterinfo());
}

void unmarshallMapCell(reader &th, map_cell& cell)
{
    unsigned flags = unmarshallUnsigned(th);
    unsigned cell_flags = 0;
    trap_type trap = TRAP_UNASSIGNED;

    cell.clear();

    switch (flags & MAP_SERIALIZE_FLAGS_MASK)
    {
    case MAP_SERIALIZE_FLAGS_8:
        cell_flags = unmarshallByte(th);
        break;
    case MAP_SERIALIZE_FLAGS_16:
        cell_flags = unmarshallShort(th);
        break;
    case MAP_SERIALIZE_FLAGS_32:
        cell_flags = unmarshallInt(th);
        break;
    }

    dungeon_feature_type feature = DNGN_UNSEEN;
    unsigned feat_colour = 0;

    if (flags & MAP_SERIALIZE_FEATURE)
#if TAG_MAJOR_VERSION == 34
        feature = unmarshallFeatureType_Info(th);
    if (feature == DNGN_SEALED_DOOR && th.getMinorVersion() < TAG_MINOR_0_12)
        feature = DNGN_CLOSED_DOOR;
    if (feature == DNGN_BADLY_SEALED_DOOR)
        feature = DNGN_SEALED_DOOR;
    if (feature == DNGN_ESCAPE_HATCH_UP
        && you.where_are_you == BRANCH_LABYRINTH)
    {
        feature = DNGN_EXIT_LABYRINTH;
    }
#else
        feature = unmarshallFeatureType(th);
#endif

    if (flags & MAP_SERIALIZE_FEATURE_COLOUR)
        feat_colour = unmarshallUnsigned(th);

    if (feat_is_trap(feature))
    {
        trap = (trap_type)unmarshallByte(th);
#if TAG_MAJOR_VERSION == 34
        if (th.getMinorVersion() == TAG_MINOR_0_11 && trap >= TRAP_TELEPORT)
            trap = (trap_type)(trap - 1);
        if (trap == TRAP_ALARM)
            feature = DNGN_TRAP_ALARM;
        else if (trap == TRAP_ZOT)
            feature = DNGN_TRAP_ZOT;
        else if (trap == TRAP_GOLUBRIA)
            feature = DNGN_PASSAGE_OF_GOLUBRIA;
#endif
    }

    cell.set_feature(feature, feat_colour, trap);

    if (flags & MAP_SERIALIZE_CLOUD)
    {
        cloud_info ci;
        ci.type = (cloud_type)unmarshallUnsigned(th);
        unmarshallUnsigned(th, ci.colour);
        unmarshallUnsigned(th, ci.duration);
        ci.tile = unmarshallShort(th);
#if TAG_MAJOR_VERSION == 34
        if (th.getMinorVersion() >= TAG_MINOR_CLOUD_OWNER)
#endif
        ci.killer = static_cast<killer_type>(unmarshallUByte(th));
        cell.set_cloud(ci);
    }

    if (flags & MAP_SERIALIZE_ITEM)
    {
        item_def item;
        unmarshallItem(th, item);
        cell.set_item(item, false);
    }

    if (flags & MAP_SERIALIZE_MONSTER)
    {
        monster_info mi;
        unmarshallMonsterInfo(th, mi);
        cell.set_monster(mi);
    }

    // set this last so the other sets don't override this
    cell.flags = cell_flags;
}

static void tag_construct_level_items(writer &th)
{
    // how many traps?
    const int nt = _last_used_index(env.trap, MAX_TRAPS);
    marshallShort(th, nt);
    for (int i = 0; i < nt; ++i)
    {
        marshallByte(th, env.trap[i].type);
        if (env.trap[i].type == TRAP_UNASSIGNED)
            continue;
        marshallCoord(th, env.trap[i].pos);
        marshallShort(th, env.trap[i].ammo_qty);
        marshallUByte(th, env.trap[i].skill_rnd);
    }

    // how many items?
    const int ni = _last_used_index(mitm, MAX_ITEMS);
    marshallShort(th, ni);
    for (int i = 0; i < ni; ++i)
        marshallItem(th, mitm[i]);
}

static void marshall_mon_enchant(writer &th, const mon_enchant &me)
{
    marshallShort(th, me.ench);
    marshallShort(th, me.degree);
    marshallShort(th, me.who);
    marshallInt(th, me.source);
    marshallShort(th, min(me.duration, INFINITE_DURATION));
    marshallShort(th, min(me.maxduration, INFINITE_DURATION));
}

static mon_enchant unmarshall_mon_enchant(reader &th)
{
    mon_enchant me;
    me.ench        = static_cast<enchant_type>(unmarshallShort(th));
    me.degree      = unmarshallShort(th);
    me.who         = static_cast<kill_category>(unmarshallShort(th));
    me.source      = unmarshallInt(th);
    me.duration    = unmarshallShort(th);
    me.maxduration = unmarshallShort(th);
    return me;
}

enum mon_part_t
{
    MP_GHOST_DEMON      = BIT(0),
    MP_CONSTRICTION     = BIT(1),
    MP_ITEMS            = BIT(2),
    MP_SPELLS           = BIT(3),
};

void marshallMonster(writer &th, const monster& m)
{
    if (!m.alive())
    {
        marshallShort(th, MONS_NO_MONSTER);
        return;
    }

    uint32_t parts = 0;
    if (mons_is_ghost_demon(m.type))
        parts |= MP_GHOST_DEMON;
    if (m.held || m.constricting && m.constricting->size())
        parts |= MP_CONSTRICTION;
    for (int i = 0; i < NUM_MONSTER_SLOTS; i++)
        if (m.inv[i] != NON_ITEM)
            parts |= MP_ITEMS;
    for (int i = 0; i < NUM_MONSTER_SPELL_SLOTS; i++)
        if (m.spells[i])
            parts |= MP_SPELLS;

    marshallShort(th, m.type);
    marshallUnsigned(th, parts);
    ASSERT(m.mid > 0);
    marshallInt(th, m.mid);
    marshallString(th, m.mname);
    marshallByte(th, m.ac);
    marshallByte(th, m.ev);
    marshallByte(th, m.get_experience_level());
    marshallByte(th, m.speed);
    marshallByte(th, m.speed_increment);
    marshallByte(th, m.behaviour);
    marshallByte(th, m.pos().x);
    marshallByte(th, m.pos().y);
    marshallByte(th, m.target.x);
    marshallByte(th, m.target.y);
    marshallCoord(th, m.firing_pos);
    marshallCoord(th, m.patrol_point);
    int help = m.travel_target;
    marshallByte(th, help);

    marshallShort(th, m.travel_path.size());
    for (unsigned int i = 0; i < m.travel_path.size(); i++)
        marshallCoord(th, m.travel_path[i]);

    marshallUnsigned(th, m.flags);
    marshallInt(th, m.experience);

    marshallShort(th, m.enchantments.size());
    for (mon_enchant_list::const_iterator i = m.enchantments.begin();
         i != m.enchantments.end(); ++i)
    {
        marshall_mon_enchant(th, i->second);
    }
    marshallByte(th, m.ench_countdown);

    marshallShort(th, min(m.hit_points, MAX_MONSTER_HP));
    marshallShort(th, min(m.max_hit_points, MAX_MONSTER_HP));
    marshallInt(th, m.number);
    marshallShort(th, m.base_monster);
    marshallShort(th, m.colour);
    marshallInt(th, m.summoner);

    if (parts & MP_ITEMS)
        for (int j = 0; j < NUM_MONSTER_SLOTS; j++)
            marshallShort(th, m.inv[j]);
    if (parts & MP_SPELLS)
        marshallSpells(th, m.spells);
    marshallByte(th, m.god);
    marshallByte(th, m.attitude);
    marshallShort(th, m.foe);
    marshallInt(th, m.foe_memory);
    marshallShort(th, m.damage_friendly);
    marshallShort(th, m.damage_total);
    marshallByte(th, m.went_unseen_this_turn);
    marshallCoord(th, m.unseen_pos);

    if (parts & MP_GHOST_DEMON)
    {
        // *Must* have ghost field set.
        ASSERT(m.ghost.get());
        marshallGhost(th, *m.ghost);
    }

    if (parts & MP_CONSTRICTION)
        _marshall_constriction(th, &m);

    m.props.write(th);
}

static void _marshall_mi_attack(writer &th, const mon_attack_def &attk)
{
    marshallInt(th, attk.type);
    marshallInt(th, attk.flavour);
    marshallInt(th, attk.damage);
}

static mon_attack_def _unmarshall_mi_attack(reader &th)
{
    mon_attack_def attk;
    attk.type = static_cast<attack_type>(unmarshallInt(th));
    attk.flavour = static_cast<attack_flavour>(unmarshallInt(th));
    attk.damage = unmarshallInt(th);

    return attk;
}

void marshallMonsterInfo(writer &th, const monster_info& mi)
{
    marshallFixedBitVector<NUM_MB_FLAGS>(th, mi.mb);
    marshallString(th, mi.mname);
#if TAG_MAJOR_VERSION == 34
    marshallUnsigned(th, mi.type);
    marshallUnsigned(th, mi.base_type);
    if (mons_genus(mi.type) == MONS_DRACONIAN
        || mons_genus(mi.type) == MONS_DEMONSPAWN)
    {
        marshallUnsigned(th, mi.draco_type);
    }
#else
    marshallShort(th, mi.type);
    marshallShort(th, mi.base_type);
    if (mons_genus(mi.type) == MONS_DRACONIAN
        || mons_genus(mi.type) == MONS_DEMONSPAWN)
    {
        marshallShort(th, mi.draco_type);
    }
#endif
    marshallUnsigned(th, mi.number);
    marshallUnsigned(th, mi.colour);
    marshallUnsigned(th, mi.attitude);
    marshallUnsigned(th, mi.threat);
    marshallUnsigned(th, mi.dam);
    marshallUnsigned(th, mi.fire_blocker);
    marshallString(th, mi.description);
    marshallString(th, mi.quote);
    marshallUnsigned(th, mi.holi);
    marshallUnsigned(th, mi.mintel);
    marshallInt(th, mi.mresists);
    marshallUnsigned(th, mi.mitemuse);
    marshallByte(th, mi.mbase_speed);
    marshallByte(th, mi.menergy.move);
    marshallByte(th, mi.menergy.swim);
    marshallByte(th, mi.menergy.attack);
    marshallByte(th, mi.menergy.missile);
    marshallByte(th, mi.menergy.spell);
    marshallByte(th, mi.menergy.special);
    marshallByte(th, mi.menergy.item);
    marshallByte(th, mi.menergy.pickup_percent);
    marshallUnsigned(th, mi.fly);
    for (int i = 0; i < MAX_NUM_ATTACKS; ++i)
        _marshall_mi_attack(th, mi.attack[i]);
    for (unsigned int i = 0; i <= MSLOT_LAST_VISIBLE_SLOT; ++i)
    {
        if (mi.inv[i].get())
        {
            marshallBoolean(th, true);
            marshallItem(th, *mi.inv[i].get(), true);
        }
        else
            marshallBoolean(th, false);
    }
    if (mons_is_pghost(mi.type))
    {
        marshallUnsigned(th, mi.u.ghost.species);
        marshallUnsigned(th, mi.u.ghost.job);
        marshallUnsigned(th, mi.u.ghost.religion);
        marshallUnsigned(th, mi.u.ghost.best_skill);
        marshallShort(th, mi.u.ghost.best_skill_rank);
        marshallShort(th, mi.u.ghost.xl_rank);
        marshallShort(th, mi.u.ghost.damage);
        marshallShort(th, mi.u.ghost.ac);
    }

    mi.props.write(th);
}

void unmarshallMonsterInfo(reader &th, monster_info& mi)
{
    unmarshallFixedBitVector<NUM_MB_FLAGS>(th, mi.mb);
    mi.mname = unmarshallString(th);
#if TAG_MAJOR_VERSION == 34
    mi.type = unmarshallMonType_Info(th);
    ASSERT(!invalid_monster_type(mi.type));
    mi.base_type = unmarshallMonType_Info(th);
    if (mons_genus(mi.type) == MONS_DEMONSPAWN
        && th.getMinorVersion() < TAG_MINOR_DEMONSPAWN)
    {
        mi.draco_type = mi.base_type;
    }
    else if (mons_genus(mi.type) == MONS_DRACONIAN
        || (mons_genus(mi.type) == MONS_DEMONSPAWN
            && th.getMinorVersion() >= TAG_MINOR_DEMONSPAWN))
    {
        mi.draco_type = unmarshallMonType_Info(th);
    }
#else
    mi.type = unmarshallMonType(th);
    ASSERT(!invalid_monster_type(mi.type));
    mi.base_type = unmarshallMonType(th);
    if (mons_genus(mi.type) == MONS_DRACONIAN
        || mons_genus(mi.type) == MONS_DEMONSPAWN)
    {
        mi.draco_type = unmarshallMonType(th);
    }
#endif
    unmarshallUnsigned(th, mi.number);
    unmarshallUnsigned(th, mi.colour);
    unmarshallUnsigned(th, mi.attitude);
    unmarshallUnsigned(th, mi.threat);
    unmarshallUnsigned(th, mi.dam);
    unmarshallUnsigned(th, mi.fire_blocker);
    mi.description = unmarshallString(th);
    mi.quote = unmarshallString(th);

    unmarshallUnsigned(th, mi.holi);
    unmarshallUnsigned(th, mi.mintel);
    mi.mresists = unmarshallInt(th);
#if TAG_MAJOR_VERSION == 34
    if (mi.mresists & MR_OLD_RES_ACID)
        set_resist(mi.mresists, MR_RES_ACID, 3);
#endif
    unmarshallUnsigned(th, mi.mitemuse);
    mi.mbase_speed = unmarshallByte(th);

#if TAG_MAJOR_VERSION == 34
    // See comment in unmarshallMonster(): this could be an elemental
    // wellspring masquerading as a spectral weapon, or a polymoth
    // masquerading as a wellspring.
    if (th.getMinorVersion() < TAG_MINOR_CANARIES
        && th.getMinorVersion() >= TAG_MINOR_WAR_DOG_REMOVAL
        && mi.type >= MONS_SPECTRAL_WEAPON
        && mi.type <= MONS_POLYMOTH)
    {
        switch (mi.base_speed())
        {
        case 10:
            mi.type = MONS_GHOST; // wellspring
            break;
        case 12:
            mi.type = MONS_POLYMOTH;
            break;
        case 25:
        case 30:
            mi.type = MONS_SPECTRAL_WEAPON;
            break;
        default:
            die("Unexpected monster_info with type %d and speed %d",
                mi.type, mi.base_speed());
        }
    }

    // As above; this could be one of several monsters.
    if (th.getMinorVersion() < TAG_MINOR_DEMONSPAWN
        && mi.type >= MONS_MONSTROUS_DEMONSPAWN
        && mi.type <= MONS_SALAMANDER_MYSTIC)
    {
        switch (mi.colour)
        {
        case BROWN:        // monstrous demonspawn, naga ritualist
            if (mi.spells[0] == SPELL_FORCE_LANCE)
                mi.type = MONS_NAGA_RITUALIST;
            else
                mi.type = MONS_MONSTROUS_DEMONSPAWN;
            break;
        case BLUE:         // gelid demonspawn
            mi.type = MONS_GELID_DEMONSPAWN;
            break;
        case RED:          // infernal demonspawn
            mi.type = MONS_INFERNAL_DEMONSPAWN;
            break;
        case GREEN:        // putrid demonspawn
            mi.type = MONS_PUTRID_DEMONSPAWN;
            break;
        case LIGHTGRAY:    // torturous demonspawn, naga sharpshooter
            if (mi.spells[0] == SPELL_PORTAL_PROJECTILE)
                mi.type = MONS_NAGA_SHARPSHOOTER;
            else
                mi.type = MONS_TORTUROUS_DEMONSPAWN;
            break;
        case LIGHTBLUE:    // blood saint, shock serpent
            if (mi.base_type != MONS_NO_MONSTER)
                mi.type = MONS_BLOOD_SAINT;
            else
                mi.type = MONS_SHOCK_SERPENT;
            break;
        case ETC_RANDOM:   // chaos champion
            mi.type = MONS_CHAOS_CHAMPION;
            break;
        case LIGHTCYAN:    // warmonger, drowned soul
            if (mi.base_type != MONS_NO_MONSTER)
                mi.type = MONS_WARMONGER;
            else
                mi.type = MONS_DROWNED_SOUL;
            break;
        case LIGHTGREEN:   // corrupter
            mi.type = MONS_CORRUPTER;
            break;
        case LIGHTMAGENTA: // black sun
            mi.type = MONS_BLACK_SUN;
            break;
        case CYAN:         // worldbinder
            mi.type = MONS_WORLDBINDER;
            break;
        case MAGENTA:      // vine stalker, mana viper, grand avatar
            if (mi.base_speed() == 30)
                mi.type = MONS_GRAND_AVATAR;
            else
                mi.type = MONS_MANA_VIPER;
            break;
        case WHITE:        // salamander firebrand
            mi.type = MONS_SALAMANDER_FIREBRAND;
            break;
        case YELLOW:       // salamander mystic
            mi.type = MONS_SALAMANDER_MYSTIC;
            break;
        default:
            die("Unexpected monster with type %d and colour %d",
                mi.type, mi.colour);
        }
        if (mons_is_demonspawn(mi.type)
            && mons_species(mi.type) == MONS_DEMONSPAWN
            && mi.type != MONS_DEMONSPAWN)
        {
            ASSERT(mi.base_type != MONS_NO_MONSTER);
        }
    }

    if (th.getMinorVersion() < TAG_MINOR_MONINFO_ENERGY)
        mi.menergy = mons_class_energy(mi.type);
    else
    {
#endif
    mi.menergy.move = unmarshallByte(th);
    mi.menergy.swim = unmarshallByte(th);
    mi.menergy.attack = unmarshallByte(th);
    mi.menergy.missile = unmarshallByte(th);
    mi.menergy.spell = unmarshallByte(th);
    mi.menergy.special = unmarshallByte(th);
    mi.menergy.item = unmarshallByte(th);
    mi.menergy.pickup_percent = unmarshallByte(th);
#if TAG_MAJOR_VERSION == 34
    }
#endif

    // Some TAG_MAJOR_VERSION == 34 saves suffered data loss here, beware.
    // Should be harmless, hopefully.
    unmarshallUnsigned(th, mi.fly);
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_ATTACK_DESCS)
    {
        for (int i = 0; i < MAX_NUM_ATTACKS; ++i)
        {
            mi.attack[i] = get_monster_data(mi.type)->attack[i];
            mi.attack[i].damage = 0;
        }
    }
    else
#endif
    for (int i = 0; i < MAX_NUM_ATTACKS; ++i)
        mi.attack[i] = _unmarshall_mi_attack(th);

    for (unsigned int i = 0; i <= MSLOT_LAST_VISIBLE_SLOT; ++i)
    {
        if (unmarshallBoolean(th))
        {
            mi.inv[i].reset(new item_def());
            unmarshallItem(th, *mi.inv[i].get());
        }
    }

    if (mons_is_pghost(mi.type))
    {
        unmarshallUnsigned(th, mi.u.ghost.species);
        unmarshallUnsigned(th, mi.u.ghost.job);
        unmarshallUnsigned(th, mi.u.ghost.religion);
        unmarshallUnsigned(th, mi.u.ghost.best_skill);
        mi.u.ghost.best_skill_rank = unmarshallShort(th);
        mi.u.ghost.xl_rank = unmarshallShort(th);
        mi.u.ghost.damage = unmarshallShort(th);
        mi.u.ghost.ac = unmarshallShort(th);
    }

    mi.props.clear();
    mi.props.read(th);

#if TAG_MAJOR_VERSION == 34
    if (mi.type == MONS_ZOMBIE_SMALL || mi.type == MONS_ZOMBIE_LARGE)
        mi.type = MONS_ZOMBIE;
    if (mi.type == MONS_SKELETON_SMALL || mi.type == MONS_SKELETON_LARGE)
        mi.type = MONS_SKELETON;
    if (mi.type == MONS_SIMULACRUM_SMALL || mi.type == MONS_SIMULACRUM_LARGE)
        mi.type = MONS_SIMULACRUM;
    if (th.getMinorVersion() < TAG_MINOR_WAR_DOG_REMOVAL)
    {
        if (mi.type == MONS_WAR_DOG)
            mi.type = MONS_WOLF;
    }
#endif

    if (mi.type != MONS_PROGRAM_BUG && mons_species(mi.type) == MONS_PROGRAM_BUG)
    {
        mi.type = MONS_GHOST;
        mi.props.clear();
    }
}

static void tag_construct_level_monsters(writer &th)
{
    int nm = 0;
    for (int i = 0; i < MAX_MONS_ALLOC; ++i)
        if (env.mons_alloc[i] != MONS_NO_MONSTER)
            nm = i + 1;

    // how many mons_alloc?
    marshallByte(th, nm);
    for (int i = 0; i < nm; ++i)
        marshallShort(th, env.mons_alloc[i]);

    // how many monsters?
    nm = _last_used_index(menv, MAX_MONSTERS);
    marshallShort(th, nm);

    for (int i = 0; i < nm; i++)
    {
        monster& m(menv[i]);

#if defined(DEBUG) || defined(DEBUG_MONS_SCAN)
        if (m.type != MONS_NO_MONSTER)
        {
            if (invalid_monster_type(m.type))
            {
                mprf(MSGCH_ERROR, "Marshalled monster #%d %s",
                     i, m.name(DESC_PLAIN, true).c_str());
            }
            if (!in_bounds(m.pos()))
            {
                mprf(MSGCH_ERROR,
                     "Marshalled monster #%d %s out of bounds at (%d, %d)",
                     i, m.name(DESC_PLAIN, true).c_str(),
                     m.pos().x, m.pos().y);
            }
        }
#endif
        marshallMonster(th, m);
    }
}

void tag_construct_level_tiles(writer &th)
{
    // Map grids.
    // how many X?
    marshallShort(th, GXM);
    // how many Y?
    marshallShort(th, GYM);

    marshallShort(th, env.tile_names.size());
    for (unsigned int i = 0; i < env.tile_names.size(); ++i)
    {
        marshallString(th, env.tile_names[i]);
#ifdef DEBUG_TILE_NAMES
        mprf("Writing '%s' into save.", env.tile_names[i].c_str());
#endif
    }

    // flavour
    marshallShort(th, env.tile_default.wall_idx);
    marshallShort(th, env.tile_default.floor_idx);

    marshallShort(th, env.tile_default.wall);
    marshallShort(th, env.tile_default.floor);
    marshallShort(th, env.tile_default.special);

    for (int count_x = 0; count_x < GXM; count_x++)
        for (int count_y = 0; count_y < GYM; count_y++)
        {
            marshallShort(th, env.tile_flv[count_x][count_y].wall_idx);
            marshallShort(th, env.tile_flv[count_x][count_y].floor_idx);
            marshallShort(th, env.tile_flv[count_x][count_y].feat_idx);

            marshallShort(th, env.tile_flv[count_x][count_y].wall);
            marshallShort(th, env.tile_flv[count_x][count_y].floor);
            marshallShort(th, env.tile_flv[count_x][count_y].feat);
            marshallShort(th, env.tile_flv[count_x][count_y].special);
        }

    marshallInt(th, TILE_WALL_MAX);
}

static void tag_read_level(reader &th)
{
    env.floor_colour = unmarshallUByte(th);
    env.rock_colour  = unmarshallUByte(th);

    env.level_flags  = unmarshallInt(th);

    env.elapsed_time = unmarshallInt(th);
    env.old_player_pos = unmarshallCoord(th);
    env.absdepth0 = absdungeon_depth(you.where_are_you, you.depth);

    // Map grids.
    // how many X?
    const int gx = unmarshallShort(th);
    // how many Y?
    const int gy = unmarshallShort(th);
    ASSERT(gx == GXM);
    ASSERT(gy == GYM);

    env.turns_on_level = unmarshallInt(th);

    EAT_CANARY;

    env.map_seen.reset();
    for (int i = 0; i < gx; i++)
        for (int j = 0; j < gy; j++)
        {
            dungeon_feature_type feat = unmarshallFeatureType(th);
            grd[i][j] = feat;
            ASSERT(feat < NUM_FEATURES);
#if TAG_MAJOR_VERSION == 34
            if (feat == DNGN_SEALED_DOOR && th.getMinorVersion() < TAG_MINOR_0_12)
                grd[i][j] = DNGN_CLOSED_DOOR;
            if (feat == DNGN_BADLY_SEALED_DOOR)
                grd[i][j] = DNGN_SEALED_DOOR;
            if (feat == DNGN_ESCAPE_HATCH_UP
                && you.where_are_you == BRANCH_LABYRINTH)
            {
                grd[i][j] = DNGN_EXIT_LABYRINTH;
            }
#endif

            unmarshallMapCell(th, env.map_knowledge[i][j]);
            // Fixup positions
            if (env.map_knowledge[i][j].monsterinfo())
                env.map_knowledge[i][j].monsterinfo()->pos = coord_def(i, j);
            if (env.map_knowledge[i][j].cloudinfo())
                env.map_knowledge[i][j].cloudinfo()->pos = coord_def(i, j);

            env.map_knowledge[i][j].flags &= ~MAP_VISIBLE_FLAG;
            if (env.map_knowledge[i][j].seen())
                env.map_seen.set(i, j);
            env.pgrid[i][j] = unmarshallInt(th);

            mgrd[i][j] = NON_MONSTER;
            env.cgrid[i][j] = EMPTY_CLOUD;
            env.tgrid[i][j] = NON_ENTITY;
        }

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_FORGOTTEN_MAP)
        env.map_forgotten.reset();
    else
#endif
    if (unmarshallBoolean(th))
    {
        MapKnowledge *f = new MapKnowledge();
        for (int x = 0; x < GXM; x++)
            for (int y = 0; y < GYM; y++)
                unmarshallMapCell(th, (*f)[x][y]);
        env.map_forgotten.reset(f);
    }
    else
        env.map_forgotten.reset();

    env.grid_colours.init(BLACK);
    _run_length_decode(th, unmarshallByte, env.grid_colours, GXM, GYM);

    EAT_CANARY;

    env.cloud_no = 0;

    // how many clouds?
    const int num_clouds = unmarshallShort(th);
    ASSERT_RANGE(num_clouds, 0, MAX_CLOUDS + 1);
    for (int i = 0; i < num_clouds; i++)
    {
        env.cloud[i].type  = static_cast<cloud_type>(unmarshallByte(th));
        if (env.cloud[i].type == CLOUD_NONE)
            continue;
        env.cloud[i].pos.x = unmarshallByte(th);
        env.cloud[i].pos.y = unmarshallByte(th);
        env.cloud[i].decay = unmarshallShort(th);
        env.cloud[i].spread_rate = unmarshallUByte(th);
        env.cloud[i].whose = static_cast<kill_category>(unmarshallUByte(th));
        env.cloud[i].killer = static_cast<killer_type>(unmarshallUByte(th));
        env.cloud[i].source = unmarshallInt(th);
        env.cloud[i].colour = unmarshallShort(th);
        env.cloud[i].name   = unmarshallString(th);
        env.cloud[i].tile   = unmarshallString(th);
        env.cloud[i].excl_rad = unmarshallInt(th);
#if TAG_MAJOR_VERSION == 34
        // Please remove soon, after games get unstuck.
        if (!in_bounds(env.cloud[i].pos) || cell_is_solid(env.cloud[i].pos))
        {
            env.cloud[i].type = CLOUD_NONE;
            continue;
        }
#else
        ASSERT_IN_BOUNDS(env.cloud[i].pos);
#endif
        env.cgrid(env.cloud[i].pos) = i;
        env.cloud_no++;
    }
    for (int i = num_clouds; i < MAX_CLOUDS; i++)
        env.cloud[i].type = CLOUD_NONE;

    EAT_CANARY;

    // how many shops?
    const int num_shops = unmarshallShort(th);
    ASSERT_RANGE(num_shops, 0, MAX_SHOPS + 1);
    for (int i = 0; i < num_shops; i++)
    {
        env.shop[i].type  = static_cast<shop_type>(unmarshallByte(th));
        if (env.shop[i].type == SHOP_UNASSIGNED)
            continue;
#if TAG_MAJOR_VERSION == 34
        if (th.getMinorVersion() < TAG_MINOR_MISC_SHOP_CHANGE
            && env.shop[i].type == NUM_SHOPS)
        {
            // This was SHOP_MISCELLANY, which is now part of SHOP_EVOKABLES.
            env.shop[i].type = SHOP_EVOKABLES;
        }
#endif
        env.shop[i].keeper_name[0] = unmarshallUByte(th);
        env.shop[i].keeper_name[1] = unmarshallUByte(th);
        env.shop[i].keeper_name[2] = unmarshallUByte(th);
        env.shop[i].pos.x = unmarshallByte(th);
        env.shop[i].pos.y = unmarshallByte(th);
        env.shop[i].greed = unmarshallByte(th);
        env.shop[i].level = unmarshallByte(th);
        env.shop[i].shop_name = unmarshallString(th);
        env.shop[i].shop_type_name = unmarshallString(th);
        env.shop[i].shop_suffix_name = unmarshallString(th);
        env.tgrid(env.shop[i].pos) = i;
    }
    for (int i = num_shops; i < MAX_SHOPS; ++i)
        env.shop[i].type = SHOP_UNASSIGNED;

    EAT_CANARY;

    env.sanctuary_pos  = unmarshallCoord(th);
    env.sanctuary_time = unmarshallByte(th);

    env.spawn_random_rate = unmarshallInt(th);

    env.markers.read(th);

    env.properties.clear();
    env.properties.read(th);

    env.dactions_done = unmarshallInt(th);

    // Restore heightmap
    env.heightmap.reset(NULL);
    const bool have_heightmap = unmarshallBoolean(th);
    if (have_heightmap)
    {
        env.heightmap.reset(new grid_heightmap);
        grid_heightmap &heightmap(*env.heightmap);
        for (rectangle_iterator ri(0); ri; ++ri)
            heightmap(*ri) = unmarshallShort(th);
    }

    EAT_CANARY;

    env.forest_awoken_until = unmarshallInt(th);
    unmarshall_level_vault_data(th);
    env.density = unmarshallInt(th);

    int num_lights = unmarshallShort(th);
    ASSERT(num_lights >= 0);
    env.sunlight.clear();
    while (num_lights-- > 0)
    {
        coord_def c = unmarshallCoord(th);
        env.sunlight.push_back(pair<coord_def, int>(c, unmarshallInt(th)));
    }
}

static void tag_read_level_items(reader &th)
{
    // how many traps?
    const int trap_count = unmarshallShort(th);
    ASSERT_RANGE(trap_count, 0, MAX_TRAPS + 1);
    for (int i = 0; i < trap_count; ++i)
    {
        env.trap[i].type =
            static_cast<trap_type>(unmarshallUByte(th));
        if (env.trap[i].type == TRAP_UNASSIGNED)
            continue;
        env.trap[i].pos      = unmarshallCoord(th);
        env.trap[i].ammo_qty = unmarshallShort(th);
#if TAG_MAJOR_VERSION == 34
        if (th.getMinorVersion() == TAG_MINOR_0_11 && env.trap[i].type >= TRAP_TELEPORT)
            env.trap[i].type = (trap_type)(env.trap[i].type - 1);
        if (th.getMinorVersion() < TAG_MINOR_TRAPS_DETERM
            || th.getMinorVersion() == TAG_MINOR_0_11)
        {
            env.trap[i].skill_rnd = random2(256);
        }
        else
#endif
        env.trap[i].skill_rnd = unmarshallUByte(th);
        env.tgrid(env.trap[i].pos) = i;
    }
    for (int i = trap_count; i < MAX_TRAPS; ++i)
        env.trap[i].type = TRAP_UNASSIGNED;

    // how many items?
    const int item_count = unmarshallShort(th);
    ASSERT_RANGE(item_count, 0, MAX_ITEMS + 1);
    for (int i = 0; i < item_count; ++i)
        unmarshallItem(th, mitm[i]);
    for (int i = item_count; i < MAX_ITEMS; ++i)
        mitm[i].clear();

#ifdef DEBUG_ITEM_SCAN
    // There's no way to fix this, even with wizard commands, so get
    // rid of it when restoring the game.
    for (int i = 0; i < MAX_ITEMS; i++)
    {
        item_def &item(mitm[i]);

        if (item.pos.origin())
            item.clear();
    }
#endif
}

void unmarshallMonster(reader &th, monster& m)
{
    m.reset();

    m.type           = unmarshallMonType(th);
    if (m.type == MONS_NO_MONSTER)
        return;

    ASSERT(!invalid_monster_type(m.type));

#if TAG_MAJOR_VERSION == 34
    uint32_t parts    = 0;
    if (th.getMinorVersion() < TAG_MINOR_MONSTER_PARTS)
    {
        if (mons_is_ghost_demon(m.type))
            parts |= MP_GHOST_DEMON;
    }
    else
        parts         = unmarshallUnsigned(th);
    if (th.getMinorVersion() < TAG_MINOR_OPTIONAL_PARTS)
        parts |= MP_CONSTRICTION | MP_ITEMS | MP_SPELLS;
#else
    uint32_t parts    = unmarshallUnsigned(th);
#endif
    m.mid             = unmarshallInt(th);
    ASSERT(m.mid > 0);
    m.mname           = unmarshallString(th);
    m.ac              = unmarshallByte(th);
    m.ev              = unmarshallByte(th);
    m.set_hit_dice(     unmarshallByte(th));
#if TAG_MAJOR_VERSION == 34
    // Draining used to be able to take a monster to 0 HD, but that
    // caused crashes if they tried to cast spells.
    m.set_hit_dice(max(m.get_experience_level(), 1));
#else
    ASSERT(m.get_experience_level() > 0);
#endif
    m.speed           = unmarshallByte(th);
    // Avoid sign extension when loading files (Elethiomel's hang)
    m.speed_increment = unmarshallUByte(th);
    m.behaviour       = static_cast<beh_type>(unmarshallUByte(th));
    int x             = unmarshallByte(th);
    int y             = unmarshallByte(th);
    m.set_position(coord_def(x,y));
    m.target.x        = unmarshallByte(th);
    m.target.y        = unmarshallByte(th);

    m.firing_pos      = unmarshallCoord(th);
    m.patrol_point    = unmarshallCoord(th);

    int help = unmarshallByte(th);
    m.travel_target = static_cast<montravel_target_type>(help);

    const int len = unmarshallShort(th);
    for (int i = 0; i < len; ++i)
        m.travel_path.push_back(unmarshallCoord(th));

    m.flags      = unmarshallUnsigned(th);
    m.experience = unmarshallInt(th);

    m.enchantments.clear();
    const int nenchs = unmarshallShort(th);
    for (int i = 0; i < nenchs; ++i)
    {
        mon_enchant me = unmarshall_mon_enchant(th);
        m.enchantments[me.ench] = me;
        m.ench_cache.set(me.ench, true);
    }
    m.ench_countdown = unmarshallByte(th);

    m.hit_points     = unmarshallShort(th);
    m.max_hit_points = unmarshallShort(th);
    m.number         = unmarshallInt(th);
    m.base_monster   = unmarshallMonType(th);
    m.colour         = unmarshallShort(th);
#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_SUMMONER)
        m.summoner = 0;
    else
#endif
    m.summoner       = unmarshallInt(th);

    if (parts & MP_ITEMS)
        for (int j = 0; j < NUM_MONSTER_SLOTS; j++)
            m.inv[j] = unmarshallShort(th);

    if (parts & MP_SPELLS)
        unmarshallSpells(th, m.spells);

    m.god      = static_cast<god_type>(unmarshallByte(th));
    m.attitude = static_cast<mon_attitude_type>(unmarshallByte(th));
    m.foe      = unmarshallShort(th);
    m.foe_memory = unmarshallInt(th);

    m.damage_friendly = unmarshallShort(th);
    m.damage_total = unmarshallShort(th);

#if TAG_MAJOR_VERSION == 34
    if (th.getMinorVersion() < TAG_MINOR_UNSEEN_MONSTER)
    {
        m.went_unseen_this_turn = false;
        m.unseen_pos = coord_def(0, 0);
    }
    else
    {
#endif
    m.went_unseen_this_turn = unmarshallByte(th);
    m.unseen_pos = unmarshallCoord(th);
#if TAG_MAJOR_VERSION == 34
    }
#endif

#if TAG_MAJOR_VERSION == 34
    if (m.type == MONS_LABORATORY_RAT)
        unmarshallGhost(th), m.type = MONS_RAT;

    // MONS_SPECTRAL_WEAPON was inserted into the wrong place
    // (0.13-a0-1964-g2fab1c1, merged into trunk in 0.13-a0-1981-g9e80fb2),
    // and then had a ghost_demon structure added (0.13-a0-2055-g6cfaa00).
    // Neither event had an associated tag, but both were between the
    // same two adjacent tags.
    if (th.getMinorVersion() < TAG_MINOR_CANARIES
        && th.getMinorVersion() >= TAG_MINOR_WAR_DOG_REMOVAL
        && m.type >= MONS_SPECTRAL_WEAPON
        && m.type <= MONS_POLYMOTH)
    {
        // But fortunately the three monsters it could be all have different
        // speeds, and none of those speeds are 3/2 or 2/3 any others. We will
        // assume that none of these had the wretched enchantment. Ugh.
        switch (m.speed)
        {
        case 6: case 7: // slowed
        case 10:
        case 15: // hasted/berserked
            m.type = MONS_GHOST; // wellspring
            break;
        case 8: // slowed
        case 12:
        case 18: // hasted/berserked
            m.type = MONS_POLYMOTH;
            break;
        case 16: case 17: case 20: // slowed
        case 25:
        case 30:
        case 37: case 38: case 45: // hasted/berserked
            m.type = MONS_SPECTRAL_WEAPON;
            break;
        default:
            die("Unexpected monster with type %d and speed %d",
                m.type, m.speed);
        }
    }

    // Spectral weapons became speed 30 in the commit immediately preceding
    // the one that added the ghost_demon. Since the commits were in the
    // same batch, no one should have saves where the speed is 30 and the
    // spectral weapon didn't have a ghost_demon, or where the speed is
    // 25 and it did.
    if (th.getMinorVersion() < TAG_MINOR_CANARIES
        && m.type == MONS_SPECTRAL_WEAPON
        // normal, slowed, and hasted, respectively.
        && m.speed != 30 && m.speed != 20 && m.speed != 45)
    {
        // Don't bother trying to fix it up.
        m.type = MONS_WOOD_GOLEM; // anything removed
        m.mid = ++you.last_mid;   // sabotage the bond
        parts &= MP_GHOST_DEMON;
    }
    else if (mons_class_is_chimeric(m.type)
             && th.getMinorVersion() < TAG_MINOR_CHIMERA_GHOST_DEMON)
    {
        // Don't unmarshall the ghost demon if this is an invalid chimera
    }
    else if (th.getMinorVersion() < TAG_MINOR_DEMONSPAWN
             && m.type >= MONS_MONSTROUS_DEMONSPAWN
             && m.type <= MONS_SALAMANDER_MYSTIC)
    {
        // The demonspawn-enemies branch was merged in such a fashion
        // that it bumped several monster enums (see merge commit:
        // 0.14-a0-2321-gdab6825).
        // Try to figure out what it is.
        switch (m.colour)
        {
        case BROWN:        // monstrous demonspawn, naga ritualist
            if (m.spells[0] == SPELL_FORCE_LANCE)
                m.type = MONS_NAGA_RITUALIST;
            else
                m.type = MONS_MONSTROUS_DEMONSPAWN;
            break;
        case BLUE:         // gelid demonspawn
            m.type = MONS_GELID_DEMONSPAWN;
            break;
        case RED:          // infernal demonspawn
            m.type = MONS_INFERNAL_DEMONSPAWN;
            break;
        case GREEN:        // putrid demonspawn
            m.type = MONS_PUTRID_DEMONSPAWN;
            break;
        case LIGHTGRAY:    // torturous demonspawn, naga sharpshooter
            if (m.spells[0] == SPELL_PORTAL_PROJECTILE)
                m.type = MONS_NAGA_SHARPSHOOTER;
            else
                m.type = MONS_TORTUROUS_DEMONSPAWN;
            break;
        case LIGHTBLUE:    // blood saint, shock serpent
            if (m.base_monster != MONS_NO_MONSTER)
                m.type = MONS_BLOOD_SAINT;
            else
                m.type = MONS_SHOCK_SERPENT;
            break;
        case ETC_RANDOM:   // chaos champion
            m.type = MONS_CHAOS_CHAMPION;
            break;
        case LIGHTCYAN:    // warmonger, drowned soul
            if (m.base_monster != MONS_NO_MONSTER)
                m.type = MONS_WARMONGER;
            else
                m.type = MONS_DROWNED_SOUL;
            break;
        case LIGHTGREEN:   // corrupter
            m.type = MONS_CORRUPTER;
            break;
        case LIGHTMAGENTA: // black sun
            m.type = MONS_BLACK_SUN;
            break;
        case CYAN:         // worldbinder
            m.type = MONS_WORLDBINDER;
            break;
        case MAGENTA:      // vine stalker, mana viper, grand avatar
            switch (m.speed)
            {
                case 20:
                case 30:
                case 45:
                    m.type = MONS_GRAND_AVATAR;
                    break;
                case 9:
                case 10:
                case 14:
                case 21:
                    m.type = MONS_MANA_VIPER;
                    break;
                default:
                    die("Unexpected monster with type %d and speed %d",
                        m.type, m.speed);
            }
            break;
        case WHITE:        // salamander firebrand
            m.type = MONS_SALAMANDER_FIREBRAND;
            break;
        case YELLOW:       // salamander mystic
            m.type = MONS_SALAMANDER_MYSTIC;
            break;
        default:
            die("Unexpected monster with type %d and colour %d",
                m.type, m.colour);
        }
        if (mons_is_demonspawn(m.type)
            && mons_species(m.type) == MONS_DEMONSPAWN
            && m.type != MONS_DEMONSPAWN)
        {
            ASSERT(m.base_monster != MONS_NO_MONSTER);
        }
    }
    // Turn elephant slugs into ghosts because they are dummies now.
    else if (m.type == MONS_ELEPHANT_SLUG)
        m.type = MONS_GHOST;
    else
#endif
    if (parts & MP_GHOST_DEMON)
        m.set_ghost(unmarshallGhost(th));
    if (parts & MP_CONSTRICTION)
        _unmarshall_constriction(th, &m);

    m.props.clear();
    m.props.read(th);

#if TAG_MAJOR_VERSION == 34
    // Now we've got props, can construct a missing ghost demon for
    // chimera that need upgrading
    if (th.getMinorVersion() < TAG_MINOR_CHIMERA_GHOST_DEMON
        && mons_is_ghost_demon(m.type)
        && mons_class_is_chimeric(m.type))
    {
        // Construct a new chimera ghost demon from the old parts
        ghost_demon ghost;
        monster_type cparts[] =
        {
            get_chimera_part(&m, 1),
            get_chimera_part(&m, 2),
            get_chimera_part(&m, 3)
        };
        ghost.init_chimera(&m, cparts);
        m.set_ghost(ghost);
        m.ghost_demon_init();
        parts |= MP_GHOST_DEMON;
    }
#endif

    if (m.props.exists("monster_tile_name"))
    {
        string tile = m.props["monster_tile_name"].get_string();
        tileidx_t index;
        if (!tile_player_index(tile.c_str(), &index))
        {
            // If invalid tile name, complain and discard the props.
            dprf("bad tile name: \"%s\".", tile.c_str());
            m.props.erase("monster_tile_name");
            if (m.props.exists("monster_tile"))
                m.props.erase("monster_tile");
        }
        else // Update monster tile.
            m.props["monster_tile"] = short(index);
    }

#if TAG_MAJOR_VERSION == 34
    // Battlespheres that don't know their creator's mid must have belonged
    // to the player pre-monster-battlesphere.
    if (th.getMinorVersion() < TAG_MINOR_BATTLESPHERE_MID
        && m.type == MONS_BATTLESPHERE && !m.props.exists("bs_mid"))
    {
        // It must have belonged to the player.
        m.summoner = MID_PLAYER;
    }
    else if (m.props.exists("bs_mid"))
    {
        m.summoner = m.props["bs_mid"].get_int();
        m.props.erase("bs_mid");
    }

    if (m.props.exists("iood_mid"))
        m.summoner = m.props["iood_mid"].get_int(), m.props.erase("iood_mid");

    if (m.type == MONS_ZOMBIE_SMALL || m.type == MONS_ZOMBIE_LARGE)
        m.type = MONS_ZOMBIE;
    if (m.type == MONS_SKELETON_SMALL || m.type == MONS_SKELETON_LARGE)
        m.type = MONS_SKELETON;
    if (m.type == MONS_SIMULACRUM_SMALL || m.type == MONS_SIMULACRUM_LARGE)
        m.type = MONS_SIMULACRUM;

    if (th.getMinorVersion() < TAG_MINOR_WAR_DOG_REMOVAL)
    {
        if (m.type == MONS_WAR_DOG)
            m.type = MONS_WOLF;
    }

    if (m.props.exists("no_hide"))
        m.props[NEVER_HIDE_KEY] = true;
#endif

    if (m.type != MONS_PROGRAM_BUG && mons_species(m.type) == MONS_PROGRAM_BUG)
    {
        m.type = MONS_GHOST;
        m.props.clear();
    }

    // If an upgrade synthesizes ghost_demon, please mark it in "parts" above.
    ASSERT(parts & MP_GHOST_DEMON || !mons_is_ghost_demon(m.type));

    m.check_speed();
}

static void tag_read_level_monsters(reader &th)
{
    int i;
    int count;

    reset_all_monsters();

    // how many mons_alloc?
    count = unmarshallByte(th);
    ASSERT(count >= 0);
    for (i = 0; i < count && i < MAX_MONS_ALLOC; ++i)
        env.mons_alloc[i] = unmarshallMonType(th);
    for (i = MAX_MONS_ALLOC; i < count; ++i)
        unmarshallShort(th);
    for (i = count; i < MAX_MONS_ALLOC; ++i)
        env.mons_alloc[i] = MONS_NO_MONSTER;

    // how many monsters?
    count = unmarshallShort(th);
    ASSERT_RANGE(count, 0, MAX_MONSTERS + 1);

    for (i = 0; i < count; i++)
    {
        monster& m = menv[i];
        unmarshallMonster(th, m);

        // place monster
        if (!m.alive())
            continue;

        // companion_is_elsewhere checks the mid cache
        env.mid_cache[m.mid] = i;
        if (m.is_divine_companion() && companion_is_elsewhere(m.mid))
        {
            dprf("Killed elsewhere companion %s(%d) on %s",
                    m.name(DESC_PLAIN, true).c_str(), m.mid,
                    level_id::current().describe(false, true).c_str());
            monster_die(&m, KILL_RESET, -1, true, false);
            continue;
        }

#if defined(DEBUG) || defined(DEBUG_MONS_SCAN)
        if (invalid_monster_type(m.type))
        {
            mprf(MSGCH_ERROR, "Unmarshalled monster #%d %s",
                 i, m.name(DESC_PLAIN, true).c_str());
        }
        if (!in_bounds(m.pos()))
        {
            mprf(MSGCH_ERROR,
                 "Unmarshalled monster #%d %s out of bounds at (%d, %d)",
                 i, m.name(DESC_PLAIN, true).c_str(),
                 m.pos().x, m.pos().y);
        }
        int midx = mgrd(m.pos());
        if (midx != NON_MONSTER)
        {
            mprf(MSGCH_ERROR, "(%d, %d) for %s already occupied by %s",
                 m.pos().x, m.pos().y,
                 m.name(DESC_PLAIN, true).c_str(),
                 menv[midx].name(DESC_PLAIN, true).c_str());
        }
#endif
        mgrd(m.pos()) = i;
    }
#if TAG_MAJOR_VERSION == 34
    // This relies on TAG_YOU (including lost monsters) being unmarshalled
    // on game load before the initial level.
    if (th.getMinorVersion() < TAG_MINOR_FIXED_CONSTRICTION
        && th.getMinorVersion() >= TAG_MINOR_OPTIONAL_PARTS)
    {
        _fix_missing_constrictions();
    }
#endif
}

static void _debug_count_tiles()
{
#ifdef DEBUG_DIAGNOSTICS
# ifdef USE_TILE
    map<int,bool> found;
    int t, cnt = 0;
    for (int i = 0; i < GXM; i++)
        for (int j = 0; j < GYM; j++)
        {
            t = env.tile_bk_bg[i][j];
            if (!found.count(t))
                cnt++, found[t] = true;
            t = env.tile_bk_fg[i][j];
            if (!found.count(t))
                cnt++, found[t] = true;
            t = env.tile_bk_cloud[i][j];
            if (!found.count(t))
                cnt++, found[t] = true;
        }
    dprf("Unique tiles found: %d", cnt);
# endif
#endif
}

void tag_read_level_tiles(reader &th)
{
    // Map grids.
    // how many X?
    const int gx = unmarshallShort(th);
    // how many Y?
    const int gy = unmarshallShort(th);

    env.tile_names.clear();
    unsigned int num_tilenames = unmarshallShort(th);
    for (unsigned int i = 0; i < num_tilenames; ++i)
    {
#ifdef DEBUG_TILE_NAMES
        string temp = unmarshallString(th);
        mprf("Reading tile_names[%d] = %s", i, temp.c_str());
        env.tile_names.push_back(temp);
#else
        env.tile_names.push_back(unmarshallString(th));
#endif
    }

    // flavour
    env.tile_default.wall_idx  = unmarshallShort(th);
    env.tile_default.floor_idx = unmarshallShort(th);
    env.tile_default.wall      = unmarshallShort(th);
    env.tile_default.floor     = unmarshallShort(th);
    env.tile_default.special   = unmarshallShort(th);

    for (int x = 0; x < gx; x++)
        for (int y = 0; y < gy; y++)
        {
            env.tile_flv[x][y].wall_idx  = unmarshallShort(th);
            env.tile_flv[x][y].floor_idx = unmarshallShort(th);
            env.tile_flv[x][y].feat_idx  = unmarshallShort(th);

            // These get overwritten by _regenerate_tile_flavour
            env.tile_flv[x][y].wall    = unmarshallShort(th);
            env.tile_flv[x][y].floor   = unmarshallShort(th);
            env.tile_flv[x][y].feat    = unmarshallShort(th);
            env.tile_flv[x][y].special = unmarshallShort(th);
        }

    _debug_count_tiles();

    _regenerate_tile_flavour();

    // Draw remembered map
    _draw_tiles();
}

static tileidx_t _get_tile_from_vector(const unsigned int idx)
{
    if (idx <= 0 || idx > env.tile_names.size())
    {
#ifdef DEBUG_TILE_NAMES
        mprf("Index out of bounds: idx = %d - 1, size(tile_names) = %d",
            idx, env.tile_names.size());
#endif
        return 0;
    }
    string tilename = env.tile_names[idx - 1];

    tileidx_t tile;
    if (!tile_dngn_index(tilename.c_str(), &tile))
    {
#ifdef DEBUG_TILE_NAMES
        mprf("tilename %s (index %d) not found",
             tilename.c_str(), idx - 1);
#endif
        return 0;
    }
#ifdef DEBUG_TILE_NAMES
    mprf("tilename %s (index %d) resolves to tile %d",
         tilename.c_str(), idx - 1, (int) tile);
#endif

    return tile;
}

static void _regenerate_tile_flavour()
{
    /* Remember the wall_idx and floor_idx; tile_init_default_flavour
       sets them to 0 */
    tileidx_t default_wall_idx = env.tile_default.wall_idx;
    tileidx_t default_floor_idx = env.tile_default.floor_idx;
    tile_init_default_flavour();
    if (default_wall_idx)
    {
        tileidx_t new_wall = _get_tile_from_vector(default_wall_idx);
        if (new_wall)
        {
            env.tile_default.wall_idx = default_wall_idx;
            env.tile_default.wall = new_wall;
        }
    }
    if (default_floor_idx)
    {
        tileidx_t new_floor = _get_tile_from_vector(default_floor_idx);
        if (new_floor)
        {
            env.tile_default.floor_idx = default_floor_idx;
            env.tile_default.floor = new_floor;
        }
    }

    for (rectangle_iterator ri(coord_def(0, 0), coord_def(GXM-1, GYM-1));
         ri; ++ri)
    {
        env.tile_flv(*ri).wall = 0;
        env.tile_flv(*ri).floor = 0;
        env.tile_flv(*ri).feat = 0;
        env.tile_flv(*ri).special = 0;

        if (env.tile_flv(*ri).wall_idx)
        {
            tileidx_t new_wall
                = _get_tile_from_vector(env.tile_flv(*ri).wall_idx);
            if (!new_wall)
                env.tile_flv(*ri).wall_idx = 0;
            else
                env.tile_flv(*ri).wall = new_wall;
        }
        if (env.tile_flv(*ri).floor_idx)
        {
            tileidx_t new_floor
                = _get_tile_from_vector(env.tile_flv(*ri).floor_idx);
            if (!new_floor)
                env.tile_flv(*ri).floor_idx = 0;
            else
                env.tile_flv(*ri).floor = new_floor;
        }
        if (env.tile_flv(*ri).feat_idx)
        {
            tileidx_t new_feat
                = _get_tile_from_vector(env.tile_flv(*ri).feat_idx);
            if (!new_feat)
                env.tile_flv(*ri).feat_idx = 0;
            else
                env.tile_flv(*ri).feat = new_feat;
        }
    }

    tile_new_level(true, false);
}

static void _draw_tiles()
{
#ifdef USE_TILE
    for (rectangle_iterator ri(coord_def(0, 0), coord_def(GXM-1, GYM-1));
         ri; ++ri)
    {
        tile_draw_map_cell(*ri);
    }
#endif
}
// ------------------------------- ghost tags ---------------------------- //

static void marshallSpells(writer &th, const monster_spells &spells)
{
    for (int j = 0; j < NUM_MONSTER_SPELL_SLOTS; ++j)
        marshallShort(th, spells[j]);
}

static void unmarshallSpells(reader &th, monster_spells &spells)
{
    for (int j = 0; j < NUM_MONSTER_SPELL_SLOTS; ++j)
    {
        spells[j] = unmarshallSpellType(th

#if TAG_MAJOR_VERSION == 34
            , true
#endif
            );
#if TAG_MAJOR_VERSION == 34
        if (th.getMinorVersion() < TAG_MINOR_MALMUTATE && spells[j] == SPELL_POLYMORPH)
            spells[j] = SPELL_MALMUTATE;

        if (spells[j] == SPELL_FAKE_RAKSHASA_SUMMON)
            spells[j] = SPELL_PHANTOM_MIRROR;

        if (spells[j] == SPELL_SUNRAY)
            spells[j] = SPELL_STONE_ARROW;
#endif
    }
}

static void marshallGhost(writer &th, const ghost_demon &ghost)
{
    marshallString(th, ghost.name.c_str());

    marshallShort(th, ghost.species);
    marshallShort(th, ghost.job);
    marshallByte(th, ghost.religion);
    marshallShort(th, ghost.best_skill);
    marshallShort(th, ghost.best_skill_level);
    marshallShort(th, ghost.xl);
    marshallShort(th, ghost.max_hp);
    marshallShort(th, ghost.ev);
    marshallShort(th, ghost.ac);
    marshallShort(th, ghost.damage);
    marshallShort(th, ghost.speed);
    marshallByte(th, ghost.see_invis);
    marshallShort(th, ghost.brand);
    marshallShort(th, ghost.att_type);
    marshallShort(th, ghost.att_flav);
    marshallInt(th, ghost.resists);
    marshallByte(th, ghost.spellcaster);
    marshallByte(th, ghost.cycle_colours);
    marshallByte(th, ghost.colour);
    marshallShort(th, ghost.fly);

    marshallSpells(th, ghost.spells);
}

static ghost_demon unmarshallGhost(reader &th)
{
    ghost_demon ghost;

    ghost.name             = unmarshallString(th);
    ghost.species          = static_cast<species_type>(unmarshallShort(th));
    ghost.job              = static_cast<job_type>(unmarshallShort(th));
    ghost.religion         = static_cast<god_type>(unmarshallByte(th));
    ghost.best_skill       = static_cast<skill_type>(unmarshallShort(th));
    ghost.best_skill_level = unmarshallShort(th);
    ghost.xl               = unmarshallShort(th);
    ghost.max_hp           = unmarshallShort(th);
    ghost.ev               = unmarshallShort(th);
    ghost.ac               = unmarshallShort(th);
    ghost.damage           = unmarshallShort(th);
    ghost.speed            = unmarshallShort(th);
    ghost.see_invis        = unmarshallByte(th);
    ghost.brand            = static_cast<brand_type>(unmarshallShort(th));
    ghost.att_type = static_cast<attack_type>(unmarshallShort(th));
    ghost.att_flav = static_cast<attack_flavour>(unmarshallShort(th));
    ghost.resists          = unmarshallInt(th);
#if TAG_MAJOR_VERSION == 34
    if (ghost.resists & MR_OLD_RES_ACID)
        set_resist(ghost.resists, MR_RES_ACID, 3);
#endif
    ghost.spellcaster      = unmarshallByte(th);
    ghost.cycle_colours    = unmarshallByte(th);
    ghost.colour           = unmarshallByte(th);

    ghost.fly              = static_cast<flight_type>(unmarshallShort(th));

    unmarshallSpells(th, ghost.spells);

    return ghost;
}

static void tag_construct_ghost(writer &th)
{
    // How many ghosts?
    marshallShort(th, ghosts.size());

    for (int i = 0, size = ghosts.size(); i < size; ++i)
        marshallGhost(th, ghosts[i]);
}

static void tag_read_ghost(reader &th)
{
    int nghosts = unmarshallShort(th);

    if (nghosts < 1 || nghosts > MAX_GHOSTS)
        return;

    for (int i = 0; i < nghosts; ++i)
        ghosts.push_back(unmarshallGhost(th));
}
