/*
    for tracking IP/port ranges
*/
#include "ranges.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define BUCKET_COUNT 16


/***************************************************************************
 * ???
 ***************************************************************************/
static void
todo_remove_at(struct RangeList *task, unsigned index)
{
    memmove(&task->list[index],
            &task->list[index+1],
            (task->count - index) * sizeof(task->list[index])
            );
    task->count--;
}


/***************************************************************************
 * Test if two ranges overlap
 ***************************************************************************/
static int
range_is_overlap(struct Range lhs, struct Range rhs)
{
    if (lhs.begin < rhs.begin) {
        if (lhs.end == 0xFFFFFFFF || lhs.end + 1 >= rhs.begin)
            return 1;
    }
    if (lhs.begin >= rhs.begin) {
        if (lhs.end <= rhs.end)
            return 1;
    }

    if (rhs.begin < lhs.begin) {
        if (rhs.end == 0xFFFFFFFF || rhs.end + 1 >= lhs.begin)
            return 1;
    }
    if (rhs.begin >= lhs.begin) {
        if (rhs.end <= lhs.end)
            return 1;
    }

    return 0;
}


/***************************************************************************
 * Combine two ranges, such as when they overlap.
 ***************************************************************************/
static void
range_combine(struct Range *lhs, struct Range rhs)
{
    if (lhs->begin > rhs.begin)
        lhs->begin = rhs.begin;
    if (lhs->end < rhs.end)
        lhs->end = rhs.end;
}


/***************************************************************************
 * Add the IPv4 range to our list of ranges.
 ***************************************************************************/
void
rangelist_add_range(struct RangeList *task, unsigned begin, unsigned end)
{
    unsigned i;
    struct Range range;

    range.begin = begin;
    range.end = end;

    /* auto-expand the list if necessary */
    if (task->count + 1 >= task->max) {
        unsigned new_max = task->max * 2 + 1;
        struct Range *new_list = (struct Range *)malloc(sizeof(*new_list) * new_max);
        memcpy(new_list, task->list, task->count * sizeof(*new_list));
        if (task->list)
            free(task->list);
        task->list = new_list;
        task->max = new_max;
    }

    /* See if the range overlaps any exist range already in the
     * list */
    for (i = 0; i < task->count; i++) {
        if (range_is_overlap(task->list[i], range)) {
            range_combine(&range, task->list[i]);
            todo_remove_at(task, i);
            rangelist_add_range(task, range.begin, range.end);
            return;
        }
    }

    /* Find a spot to insert in sorted order */
    for (i = 0; i < task->count; i++) {
        if (range.begin < task->list[i].begin) {
            memmove(task->list+i+1, task->list+i, (task->count - i) * sizeof(task->list[0]));
            break;
        }
    }

    /* Add to end of list */
    task->list[i].begin = begin;
    task->list[i].end = end;
    task->count++;
}

/***************************************************************************
 ***************************************************************************/
void
rangelist_remove_range(struct RangeList *task, unsigned begin, unsigned end)
{
    unsigned i;
    struct Range x;

    x.begin = begin;
    x.end = end;

    /* See if the range overlaps any exist range already in the
     * list */
    for (i = 0; i < task->count; i++) {
        if (!range_is_overlap(task->list[i], x))
            continue;

        /* If the removal-range wholly covers the range, delete
         * it completely */
        if (begin <= task->list[i].begin && end >= task->list[i].end) {
            todo_remove_at(task, i);
            i--;
            continue;
        }

        /* If the removal-range bisects the target-rage, truncate
         * the lower end and add a new high-end */
        if (begin > task->list[i].begin && end < task->list[i].end) {
            struct Range newrange;

            newrange.begin = end+1;
            newrange.end = task->list[i].end;


            task->list[i].end = begin-1;

            rangelist_add_range(task, newrange.begin, newrange.end);
            i--;
            continue;
        }

        /* If overlap on the lower side */
        if (end >= task->list[i].begin && end < task->list[i].end) {
            task->list[i].begin = end+1;
        }

        /* If overlap on the upper side */
        if (begin > task->list[i].begin && begin <= task->list[i].end) {
             task->list[i].end = begin-1;
        }

        //assert(!"impossible");
    }
}

void
rangelist_add_range2(struct RangeList *task, struct Range range)
{
    rangelist_add_range(task, range.begin, range.end);
}
void
rangelist_remove_range2(struct RangeList *task, struct Range range)
{
    rangelist_remove_range(task, range.begin, range.end);
}


/***************************************************************************
 * Parse an IPv4 address from a line of text, moving the offset forward
 * to the first non-IPv4 character
 ***************************************************************************/
static unsigned
parse_ipv4(const char *line, unsigned *inout_offset, unsigned max)
{
    unsigned offset = *inout_offset;
    unsigned result = 0;
    unsigned i;

    for (i=0; i<4; i++) {
        unsigned x = 0;
        while (offset < max && isdigit(line[offset]&0xFF)) {
            x = x * 10 + (line[offset] - '0');
            offset++;
        }
        result = result * 256 + (x & 0xFF);
        if (offset >= max || line[offset] != '.')
            break;
        offset++; /* skip dot */
    }

    *inout_offset = offset;
    return result;
}

/****************************************************************************
 * Parse from text an IPv4 address range. This can be in one of several 
 * formats:
 * - '192.168.1.1" - a single address
 * - '192.168.1.0/24" - a CIDR spec
 * - '192.168.1.0-192.168.1.255' - a range
 * @param line
 *		Part of a line of text, probably read from a commandline or conf
 *		file.
 * @param inout_offset
 *		On input, the offset from the start of the line where the address
 *		starts. On output, the offset of the first character after the
 *		range, or equal to 'max' if the line prematurely ended.
 * @param max
 *		The maximum length of the line.
 * @return
 *		The first and last address of the range, inclusive.
 ****************************************************************************/
struct Range
range_parse_ipv4(const char *line, unsigned *inout_offset, unsigned max)
{
    unsigned offset;
    struct Range result;

    if (inout_offset == NULL) {
         inout_offset = &offset;
         offset = 0;
         max = (unsigned)strlen(line);
    } else
        offset = *inout_offset;


    /* trim whitespace */
    while (offset < max && isspace(line[offset]&0xFF))
        offset++;

    /* get the first IP address */
    result.begin = parse_ipv4(line, &offset, max);
    result.end = result.begin;

    /* trim whitespace */
    while (offset < max && isspace(line[offset]&0xFF))
        offset++;

	/* If onely one IP address, return that */
	if (offset >= max)
		goto end;

    /*
	 * Handle CIDR address of the form "10.0.0.0/8" 
	 */
    if (line[offset] == '/') {
        unsigned prefix = 0;
        uint64_t mask = 0;

		/* skip slash */
        offset++;

		/* parse decimal integer */
        while (offset<max && isdigit(line[offset]&0xFF))
            prefix = prefix * 10 + (line[offset++] - '0');

		/* Create the mask from the prefix */
        mask = 0xFFFFFFFF00000000UL >> prefix;
        
		/* Mask off any non-zero bits from the start
		 * TODO print warning */
		result.begin &= mask;

		/* Set all suffix bits to 1, so that 192.168.1.0/24 has
		 * an ending address of 192.168.1.255. */
        result.end = result.begin | (unsigned)~mask;
		goto end;
    }
	
	/*
	 * Handle a dashed range like "10.0.0.100-10.0.0.200"
	 */
	if (offset<max && line[offset] == '-') {
		unsigned ip;

        offset++;
		ip = parse_ipv4(line, &offset, max);
		if (ip < result.begin) {
            result.begin = 0xFFFFFFFF;
            result.end = 0x00000000;
			fprintf(stderr, "err: ending addr %u.%u.%u.%u cannot come before starting addr %u.%u.%u.%u\n",
				((ip>>24)&0xFF), ((ip>>16)&0xFF), ((ip>>8)&0xFF), ((ip>>0)&0xFF), 
				((result.begin>>24)&0xFF), ((result.begin>>16)&0xFF), ((result.begin>>8)&0xFF), ((result.begin>>0)&0xFF)
				);
        } else
            result.end = ip;
		goto end;
	}

end:
	*inout_offset = offset;
    return result;
}


/***************************************************************************
 ***************************************************************************/
uint64_t
rangelist_count(struct RangeList *targets)
{
	unsigned i;
	uint64_t result = 0;

	for (i=0; i<targets->count; i++) {
		result += (uint64_t)targets->list[i].end - (uint64_t)targets->list[i].begin + 1UL;
	}

	return result;
}


/***************************************************************************
 * Get's the indexed port/address.
 *
 * Note that this requires a search of all the ranges. Currently, this is
 * done by a learn search of the ranges. This needs to change, because
 * once we start adding in a lot of "exclude ranges", the address space
 * will get fragmented, and the linear search will take too long.
 ***************************************************************************/
unsigned
rangelist_pick(struct RangeList *targets, uint64_t index)
{
	unsigned i;

	for (i=0; i<targets->count; i++) {
		uint64_t range = targets->list[i].end - targets->list[i].begin + 1;
		if (index < range)
			return (unsigned)(targets->list[i].begin + index);
		else
			index -= range;
	}

	assert(!"end of list");
	return 0;
}

/***************************************************************************
 ***************************************************************************/
void
rangelist_parse_ports(struct RangeList *ports, const char *string)
{
	char *p = (char*)string;

	while (*p) {
		unsigned port;
		unsigned end;

		while (*p && isspace(*p & 0xFF))
			p++;
		if (*p == 0)
			break;

		port = strtoul(p, &p, 0);
		end = port;
		if (*p == '-') {
			p++;
			end = strtoul(p, &p, 0);
		}
		if (*p == ',')
			p++;

		if (port > 0xFFFF || end > 0xFFFF || end < port) {
			fprintf(stderr, "CONF: bad ports: %u-%u\n", port, end);
			break;
		} else {
			rangelist_add_range(ports, port, end);
		}
	}
}


/***************************************************************************
 * Called during "make regress" to run a regression test over this module.
 ***************************************************************************/
int
ranges_selftest()
{
    struct Range r;
    struct RangeList task[1];

    memset(task, 0, sizeof(task[0]));


#define ERROR() fprintf(stderr, "selftest: failed %s:%u\n", __FILE__, __LINE__);

    r = range_parse_ipv4("192.168.1.3", 0, 0);
    if (r.begin != 0xc0a80103 || r.end != 0xc0a80103) {
        fprintf(stderr, "r.begin = 0x%08x r.end = 0x%08x\n", r.begin, r.end);
        ERROR();
        return 1;
    }

    r = range_parse_ipv4("10.0.0.20-10.0.0.30", 0, 0);
    if (r.begin != 0x0A000000+20 || r.end != 0x0A000000+30) {
        fprintf(stderr, "r.begin = 0x%08x r.end = 0x%08x\n", r.begin, r.end);
        ERROR();
        return 1;
    }

    r = range_parse_ipv4("10.0.1.2/16", 0, 0);
    if (r.begin != 0x0A000000 || r.end != 0x0A00FFFF) {
        fprintf(stderr, "r.begin = 0x%08x r.end = 0x%08x\n", r.begin, r.end);
        ERROR();
        return 1;
    }


    rangelist_add_range2(task, range_parse_ipv4("10.0.0.0/24", 0, 0));
    rangelist_add_range2(task, range_parse_ipv4("10.0.1.10-10.0.1.19", 0, 0));
    rangelist_add_range2(task, range_parse_ipv4("10.0.1.20-10.0.1.30", 0, 0));
    rangelist_add_range2(task, range_parse_ipv4("10.0.0.0-10.0.1.12", 0, 0));

    if (task->count != 1) {
        fprintf(stderr, "count = %u\n", task->count);
        ERROR();
        return 1;
    }
    if (task->list[0].begin != 0x0a000000 || task->list[0].end != 0x0a000100+30) {
        fprintf(stderr, "r.begin = 0x%08x r.end = 0x%08x\n", task->list[0].begin, task->list[0].end);
        ERROR();
        return 1;
    }

    /*
     * Test removal
     */
    {
        struct RangeList task[1];

        memset(task, 0, sizeof(task[0]));

        rangelist_add_range2(task, range_parse_ipv4("10.0.0.0/8", 0, 0));
        
        /* These removals shouldn't change anything */
        rangelist_remove_range2(task, range_parse_ipv4("9.255.255.255", 0, 0));
        rangelist_remove_range2(task, range_parse_ipv4("11.0.0.0/16", 0, 0));
        rangelist_remove_range2(task, range_parse_ipv4("192.168.0.0/16", 0, 0));
        if (task->count != 1 
            || task->list->begin != 0x0a000000
            || task->list->end != 0x0aFFFFFF) {
            ERROR();
            return 1;
        }

        /* These removals should remove a bit from the edges */
        rangelist_remove_range2(task, range_parse_ipv4("1.0.0.0-10.0.0.0", 0, 0));
        rangelist_remove_range2(task, range_parse_ipv4("10.255.255.255-11.0.0.0", 0, 0));
        if (task->count != 1 
            || task->list->begin != 0x0a000001
            || task->list->end != 0x0aFFFFFE) {
            ERROR();
            return 1;
        }


        /* remove things from the middle */
        rangelist_remove_range2(task, range_parse_ipv4("10.10.0.0/16", 0, 0));
        rangelist_remove_range2(task, range_parse_ipv4("10.20.0.0/16", 0, 0));
        if (task->count != 3) {
            ERROR();
            return 1;
        }

        rangelist_remove_range2(task, range_parse_ipv4("10.12.0.0/16", 0, 0));
        if (task->count != 4) {
            ERROR();
            return 1;
        }

        rangelist_remove_range2(task, range_parse_ipv4("10.10.10.10-10.12.12.12", 0, 0));
        if (task->count != 3) {
            ERROR();
            return 1;
        }

    }

    /* test ports */
    {
        struct RangeList task;

        memset(&task, 0, sizeof(task));

        rangelist_parse_ports(&task, "80,1000-2000,1234,4444");
        if (task.count != 3) {
            ERROR();
            return 1;
        }

        if (task.list[0].begin != 80 || task.list[0].end != 80 ||
            task.list[1].begin != 1000 || task.list[1].end != 2000 ||
            task.list[2].begin != 4444 || task.list[2].end != 4444) {
            ERROR();
            return 1;
        }
    }

    return 0;
}