#include <stdlib.h>
#include <memory.h>
#include <math.h>
#include "../mem.h"
#include "../types.h"
#include "../q.h"
#include "../MD5.h"
#include "poly.h"
#include "active.h"
#include "xrow.h"
#include "wind.h"
#include "convert.h"

static gfxpoly_t*current_polygon = 0;
void gfxpoly_fail(char*expr, char*file, int line, const char*function)
{
    if(!current_polygon) {
	fprintf(stderr, "assert(%s) failed in %s in line %d: %s\n", expr, file, line, function);
	exit(1);
    }

    void*md5 = init_md5();
   
    int s,t;
    gfxpolystroke_t*stroke = current_polygon->strokes;
    for(;stroke;stroke=stroke->next) {
	for(t=0;t<stroke->num_points;t++) {
	    update_md5(md5, (unsigned char*)&stroke->points[t].x, sizeof(stroke->points[t].x));
	    update_md5(md5, (unsigned char*)&stroke->points[t].y, sizeof(stroke->points[t].y));
	}
    }
    unsigned char h[16];
    char filename[32+4+1];
    finish_md5(md5, h);
    sprintf(filename, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x.ps",
	    h[0],h[1],h[2],h[3],h[4],h[5],h[6],h[7],h[8],h[9],h[10],h[11],h[12],h[13],h[14],h[15]);

    fprintf(stderr, "assert(%s) failed in %s in line %d: %s\n", expr, file, line, function);
    fprintf(stderr, "I'm saving a debug file \"%s\" to the current directory.\n", filename);

    gfxpoly_save(current_polygon, filename);
    exit(1);
}

static char point_equals(const void*o1, const void*o2)
{
    const point_t*p1 = o1;
    const point_t*p2 = o2;
    return p1->x == p2->x && p1->y == p2->y;
}
static unsigned int point_hash(const void*o)
{
    const point_t*p = o;
    return p->x^p->y;
}
static void* point_dup(const void*o)
{
    const point_t*p = o;
    point_t*n = malloc(sizeof(point_t));
    n->x = p->x;
    n->y = p->y;
    return n;
}
static void point_free(void*o)
{
    point_t*p = o;
    p->x = 0;
    p->y = 0;
    free(p);
}
static type_t point_type = {
    equals: point_equals,
    hash: point_hash,
    dup: point_dup,
    free: point_free,
};

typedef struct _status {
    int32_t y;
    actlist_t*actlist;
    heap_t*queue;
    xrow_t*xrow;
    windrule_t*windrule;
    windcontext_t*context;
    segment_t*ending_segments;
    polywriter_t writer;
#ifdef CHECKS
    dict_t*seen_crossings; //list of crossing we saw so far
    dict_t*intersecting_segs; //list of segments intersecting in this scanline
    dict_t*segs_with_point; //lists of segments that received a point in this scanline
#endif
} status_t;

typedef struct _event {
    eventtype_t type;
    point_t p;
    segment_t*s1;
    segment_t*s2;
} event_t;

/* compare_events_simple differs from compare_events in that it schedules
   events from left to right regardless of type. It's only used in horizontal
   processing, in order to get an x-wise sorting of the current scanline */
static int compare_events_simple(const void*_a,const void*_b)
{
    event_t* a = (event_t*)_a;
    event_t* b = (event_t*)_b;
    int d = b->p.y - a->p.y;
    if(d) return d;
    d = b->p.x - a->p.x;
    if(d) return d;
    return 0;
}

static int compare_events(const void*_a,const void*_b)
{
    event_t* a = (event_t*)_a;
    event_t* b = (event_t*)_b;
    int d = b->p.y - a->p.y;
    if(d) return d;
    /* we need to schedule end after intersect (so that a segment about
       to end has a chance to tear up a few other segs first) and start
       events after end (in order not to confuse the intersection check, which
       assumes there's an actual y overlap between active segments, and 
       because ending segments in the active list make it difficult to insert
       starting segments at the right position)). 
       Horizontal lines come last, because the only purpose
       they have is to create snapping coordinates for the segments (still)
       existing in this scanline.
    */
    d = b->type - a->type;
    if(d) return d;
    return 0;

    /* I don't see any reason why we would need to order by x- at least as long
       as we do horizontal lines in a seperate pass */
    //d = b->p.x - a->p.x;
    //return d;
}

int gfxpoly_size(gfxpoly_t*poly)
{
    int s,t;
    int edges = 0;
    gfxpolystroke_t*stroke = poly->strokes;
    for(;stroke;stroke=stroke->next) {
	edges += stroke->num_points-1;
    }
    return edges;
}

char gfxpoly_check(gfxpoly_t*poly)
{
    dict_t*d = dict_new2(&point_type);
    int s,t;
    gfxpolystroke_t*stroke = poly->strokes;
    for(;stroke;stroke=stroke->next) {
	for(s=0;s<stroke->num_points;s++) {
	    point_t p = stroke->points[s];
	    int num = (s>=1 && s<stroke->num_points-1)?2:1; // mid points are two points (start+end)
	    if(!dict_contains(d, &p)) {
		dict_put(d, &p, (void*)(ptroff_t)num);
	    } else {
		int count = (ptroff_t)dict_lookup(d, &p);
		dict_del(d, &p);
		count+=num;
		dict_put(d, &p, (void*)(ptroff_t)count);
	    }
	}
    }
    DICT_ITERATE_ITEMS(d, point_t*, p, void*, c) {
        int count = (ptroff_t)c;
        if(count&1) {
            fprintf(stderr, "Point (%f,%f) occurs %d times\n", p->x*poly->gridsize, p->y*poly->gridsize, count);
            dict_destroy(d);
            return 0;
        }
    }
    dict_destroy(d);
    return 1;
}

void gfxpoly_dump(gfxpoly_t*poly)
{
    int s,t;
    double g = poly->gridsize;
    fprintf(stderr, "polyon %08x (gridsize: %f)\n", poly, poly->gridsize);
    gfxpolystroke_t*stroke = poly->strokes;
    for(;stroke;stroke=stroke->next) {
	for(s=0;s<stroke->num_points-1;s++) {
	    point_t a = stroke->points[s];
	    point_t b = stroke->points[s+1];
	    fprintf(stderr, "%s(%f,%f) -> (%f,%f)%s\n", s?" ":"[", a.x*g, a.y*g, b.x*g, b.y*g,
		                                        s==stroke->num_points-2?"]":"");
	}
    }
}

void gfxpoly_save(gfxpoly_t*poly, const char*filename)
{
    FILE*fi = fopen(filename, "wb");
    fprintf(fi, "%% gridsize %f\n", poly->gridsize);
    fprintf(fi, "%% begin\n");
    int s,t;
    gfxpolystroke_t*stroke = poly->strokes;
    for(;stroke;stroke=stroke->next) {
	for(s=0;s<stroke->num_points-1;s++) {
	    point_t a = stroke->points[s];
	    point_t b = stroke->points[s+1];
	    fprintf(fi, "%g setgray\n", stroke->dir==DIR_UP ? 0.7 : 0);
	    fprintf(fi, "%d %d moveto\n", a.x, a.y);
	    fprintf(fi, "%d %d lineto\n", b.x, b.y);
	    fprintf(fi, "stroke\n");
	}
    }
    fprintf(fi, "showpage\n");
    fclose(fi);
}

inline static event_t event_new()
{
    event_t e;
    memset(&e, 0, sizeof(e));
    return e;
}

static void event_dump(event_t*e)
{
    if(e->type == EVENT_HORIZONTAL) {
        fprintf(stderr, "Horizontal [%d] (%d,%d) -> (%d,%d)\n", e->s1->nr, e->s1->a.x, e->s1->a.y, e->s1->b.x, e->s1->b.y);
    } else if(e->type == EVENT_START) {
        fprintf(stderr, "event: segment [%d] starts at (%d,%d)\n", e->s1->nr, e->p.x, e->p.y);
    } else if(e->type == EVENT_END) {
        fprintf(stderr, "event: segment [%d] ends at (%d,%d)\n", e->s1->nr, e->p.x, e->p.y);
    } else if(e->type == EVENT_CROSS) {
        fprintf(stderr, "event: segment [%d] and [%d] intersect at (%d,%d)\n", e->s1->nr, e->s2->nr, e->p.x, e->p.y);
    } else {
        assert(0);
    }
}

static inline max32(int32_t v1, int32_t v2) {return v1>v2?v1:v2;}
static inline min32(int32_t v1, int32_t v2) {return v1<v2?v1:v2;}

static void segment_dump(segment_t*s)
{
    fprintf(stderr, "[%d] (%d,%d)->(%d,%d) ", s->nr, s->a.x, s->a.y, s->b.x, s->b.y);
    fprintf(stderr, " dx:%d dy:%d k:%f dx/dy=%f\n", s->delta.x, s->delta.y, s->k,
            (double)s->delta.x / s->delta.y);
}

static void segment_init(segment_t*s, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int polygon_nr, segment_dir_t dir)
{
    s->dir = dir;
    if(y1!=y2) {
	assert(y1<y2);
    } else {
        /* up/down for horizontal segments is handled by "rotating"
           them 90° anticlockwise in screen coordinates (tilt your head to
           the right) 
           TODO: is this still needed?
	 */
        s->dir = DIR_UP;
        if(x1>x2) {
            s->dir = DIR_DOWN;
            int32_t x = x1;x1=x2;x2=x;
            int32_t y = y1;y1=y2;y2=y;
        }
    }
    s->a.x = x1;
    s->a.y = y1;
    s->b.x = x2;
    s->b.y = y2;
    s->k = (double)x1*y2-(double)x2*y1;
    s->left = s->right = 0;
    s->delta.x = x2-x1;
    s->delta.y = y2-y1;
    s->minx = min32(x1,x2);
    s->maxx = max32(x1,x2);

    s->pos = s->a;
    s->polygon_nr = polygon_nr;
    static int segment_count=0;
    s->nr = segment_count++;

#ifdef CHECKS
    assert(LINE_EQ(s->a, s) == 0);
    assert(LINE_EQ(s->b, s) == 0);

    /* check that all signs are in order:
       a        a
       |\      /|
       | \    / |
     minx-b  b--maxx
     < 0        > 0
    */
    point_t p = s->b;
    p.x = min32(s->a.x, s->b.x);
    assert(LINE_EQ(p, s) <= 0);
    p.x = max32(s->a.x, s->b.x);
    assert(LINE_EQ(p, s) >= 0);
#endif

    /* TODO: make this int_type */
    dict_init2(&s->scheduled_crossings, &ptr_type, 0);
}

static segment_t* segment_new(point_t a, point_t b, int polygon_nr, segment_dir_t dir)
{
    segment_t*s = (segment_t*)rfx_calloc(sizeof(segment_t));
    segment_init(s, a.x, a.y, b.x, b.y, polygon_nr, dir);
    return s;
}

static void segment_clear(segment_t*s)
{
    dict_clear(&s->scheduled_crossings);
}
static void segment_destroy(segment_t*s)
{
    segment_clear(s);
    free(s);
}

static void advance_stroke(heap_t*queue, gfxpolystroke_t*stroke, int polygon_nr, int pos)
{
    while(pos < stroke->num_points-1) {
	assert(stroke->points[pos].y <= stroke->points[pos+1].y);
	segment_t*s = segment_new(stroke->points[pos], stroke->points[pos+1], polygon_nr, stroke->dir);
	s->stroke = stroke;
	s->stroke_pos = ++pos;
#ifdef DEBUG
	/*if(l->tmp)
	    s->nr = l->tmp;*/
	fprintf(stderr, "[%d] (%d,%d) -> (%d,%d) %s (%d more to come)\n",
		s->nr, s->a.x, s->a.y, s->b.x, s->b.y,
		s->dir==DIR_UP?"up":"down", stroke->num_points - 1 - pos);
#endif
	event_t e = event_new();
	e.type = s->delta.y ? EVENT_START : EVENT_HORIZONTAL;
	e.p = s->a;
	e.s1 = s;
	e.s2 = 0;
	heap_put(queue, &e);
	if(e.type != EVENT_HORIZONTAL) {
	    break;
	}
    }
}

static void gfxpoly_enqueue(gfxpoly_t*p, heap_t*queue, int polygon_nr)
{
    int t;
    gfxpolystroke_t*stroke = p->strokes;
    for(;stroke;stroke=stroke->next) {
	assert(stroke->num_points > 1);

#ifdef CHECKS
	int s;
	for(s=0;s<stroke->num_points-1;s++) {
	    assert(stroke->points[s].y <= stroke->points[s+1].y);
	}
#endif
	advance_stroke(queue, stroke, polygon_nr, 0);
    }
}

static void schedule_endpoint(status_t*status, segment_t*s)
{
    // schedule end point of segment
    assert(s->b.y > status->y);
    event_t e;
    e.type = EVENT_END;
    e.p = s->b;
    e.s1 = s;
    e.s2 = 0;
    heap_put(status->queue, &e);
}

static void schedule_crossing(status_t*status, segment_t*s1, segment_t*s2)
{
    /* the code that's required (and the checks you can perform) before
       it can be said with 100% certainty that we indeed have a valid crossing
       amazes me every time. -mk */
#ifdef CHECKS
    assert(s1!=s2);
    assert(s1->right == s2);
    assert(s2->left == s1);
    int32_t miny1 = min32(s1->a.y,s1->b.y);
    int32_t maxy1 = max32(s1->a.y,s1->b.y);
    int32_t miny2 = min32(s2->a.y,s2->b.y);
    int32_t maxy2 = max32(s2->a.y,s2->b.y);
    int32_t minx1 = min32(s1->a.x,s1->b.x);
    int32_t minx2 = min32(s2->a.x,s2->b.x);
    int32_t maxx1 = max32(s1->a.x,s1->b.x);
    int32_t maxx2 = max32(s2->a.x,s2->b.x);
    /* check that precomputation is sane */
    assert(minx1 == s1->minx && minx2 == s2->minx);
    assert(maxx1 == s1->maxx && maxx2 == s2->maxx);
    /* both segments are active, so this can't happen */
    assert(!(maxy1 <= miny2 || maxy2 <= miny1));
    /* we know that right now, s2 is to the right of s1, so there's
       no way the complete bounding box of s1 is to the right of s1 */
    assert(!(s1->minx > s2->maxx));
    assert(s1->minx != s2->maxx || (!s1->delta.x && !s2->delta.x));
#endif

    if(s1->maxx <= s2->minx) {
#ifdef DEBUG
            fprintf(stderr, "[%d] doesn't intersect with [%d] because: bounding boxes don't intersect\n", s1->nr, s2->nr);
#endif
        /* bounding boxes don't intersect */
        return;
    }

#define REMEMBER_CROSSINGS
#ifdef REMEMBER_CROSSINGS
    if(dict_contains(&s1->scheduled_crossings, (void*)(ptroff_t)s2->nr)) {
        /* FIXME: this whole segment hashing thing is really slow */
#ifdef DEBUG
        fprintf(stderr, "[%d] doesn't intersect with [%d] because: we already scheduled this intersection\n", s1->nr, s2->nr);
//	DICT_ITERATE_KEY(&s1->scheduled_crossings, void*, x) {
//	    fprintf(stderr, "[%d]<->[%d]\n", s1->nr, (int)(ptroff_t)x);
//	}
#endif
        return; // we already know about this one
    }
#endif

    double det = (double)s1->delta.x*s2->delta.y - (double)s1->delta.y*s2->delta.x;
    if(!det) {
        if(s1->k == s2->k) {
            // lines are exactly on top of each other (ignored)
#ifdef DEBUG
            fprintf(stderr, "Notice: segments [%d] and [%d] are exactly on top of each other\n", s1->nr, s2->nr);
#endif
            return;
        } else {
#ifdef DEBUG
            fprintf(stderr, "[%d] doesn't intersect with [%d] because: they are parallel to each other\n", s1->nr, s2->nr);
#endif
            /* lines are parallel */
            return;
        }
    }
    double asign2 = LINE_EQ(s1->a, s2);
    double bsign2 = LINE_EQ(s1->b, s2);
    if(asign2<0 && bsign2<0) {
        // segment1 is completely to the left of segment2
#ifdef DEBUG
            fprintf(stderr, "[%d] doesn't intersect with [%d] because: [%d] is completely to the left of [%d]\n", s1->nr, s2->nr, s1->nr, s2->nr);
#endif
        return;
    }
    if(asign2>0 && bsign2>0)  {
	// TODO: can this ever happen?
#ifdef DEBUG
            fprintf(stderr, "[%d] doesn't intersect with [%d] because: [%d] is completely to the left of [%d]\n", s1->nr, s2->nr, s2->nr, s1->nr);
#endif
        // segment2 is completely to the left of segment1
        return;
    }
    if(asign2==0) {
        // segment1 touches segment2 in a single point (ignored)
#ifdef DEBUG
        fprintf(stderr, "Notice: segment [%d]'s start point touches segment [%d]\n", s1->nr, s2->nr);
#endif
        return;
    }
    if(bsign2==0) {
        // segment1 touches segment2 in a single point (ignored)
#ifdef DEBUG
        fprintf(stderr, "Notice: segment [%d]'s end point touches segment [%d]\n", s1->nr, s2->nr);
#endif
        return;
    }
    double asign1 = LINE_EQ(s2->a, s1);
    double bsign1 = LINE_EQ(s2->b, s1);
    if(asign1<0 && bsign1<0) {
        // segment1 is completely to the left of segment2
#ifdef DEBUG
            fprintf(stderr, "[%d] doesn't intersect with [%d] because: [%d] is completely to the left of [%d]\n", s1->nr, s2->nr, s1->nr, s2->nr);
#endif
        return;
    }
    if(asign1>0 && bsign1>0)  {
        // segment2 is completely to the left of segment1
#ifdef DEBUG
            fprintf(stderr, "[%d] doesn't intersect with [%d] because: [%d] is completely to the left of [%d]\n", s1->nr, s2->nr, s2->nr, s1->nr);
#endif
        return;
    }
    if(asign1==0) {
        // segment2 touches segment1 in a single point (ignored)
#ifdef DEBUG
        fprintf(stderr, "Notice: segment [%d]'s start point touches segment [%d]\n", s2->nr, s1->nr);
#endif
        return;
    }
    if(asign2==0) {
        // segment2 touches segment1 in a single point (ignored)
#ifdef DEBUG
        fprintf(stderr, "Notice: segment [%d]'s end point touches segment [%d]\n", s2->nr, s1->nr);
#endif
        return;
    }

    /* TODO: should we precompute these? */
    double la = (double)s1->a.x*(double)s1->b.y - (double)s1->a.y*(double)s1->b.x;
    double lb = (double)s2->a.x*(double)s2->b.y - (double)s2->a.y*(double)s2->b.x;

    point_t p;
    p.x = (int32_t)ceil((-la*s2->delta.x + lb*s1->delta.x) / det);
    p.y = (int32_t)ceil((+lb*s1->delta.y - la*s2->delta.y) / det);

#ifndef REMEMBER_CROSSINGS
    if(p.y < status->y) return;
#endif

    assert(p.y >= status->y);
#ifdef CHECKS
    assert(p.x >= s1->minx && p.x <= s1->maxx);
    assert(p.x >= s2->minx && p.x <= s2->maxx);

    point_t pair;
    pair.x = s1->nr;
    pair.y = s2->nr;
#ifdef REMEMBER_CROSSINGS
    assert(!dict_contains(status->seen_crossings, &pair));
    dict_put(status->seen_crossings, &pair, 0);
#endif
#endif
#ifdef DEBUG
    fprintf(stderr, "schedule crossing between [%d] and [%d] at (%d,%d)\n", s1->nr, s2->nr, p.x, p.y);
#endif

#ifdef REMEMBER_CROSSINGS
    /* we insert into each other's intersection history because these segments might switch
       places and we still want to look them up quickly after they did */
    dict_put(&s1->scheduled_crossings, (void*)(ptroff_t)(s2->nr), 0);
    dict_put(&s2->scheduled_crossings, (void*)(ptroff_t)(s1->nr), 0);
#endif

    event_t e = event_new();
    e.type = EVENT_CROSS;
    e.p = p;
    e.s1 = s1;
    e.s2 = s2;
    heap_put(status->queue, &e);
    return;
}

static void exchange_two(status_t*status, event_t*e)
{
    //exchange two segments in list
    segment_t*s1 = e->s1;
    segment_t*s2 = e->s2;
#ifdef CHECKS
    if(!dict_contains(status->intersecting_segs, s1))
        dict_put(status->intersecting_segs, s1, 0);
    if(!dict_contains(status->intersecting_segs, s2))
        dict_put(status->intersecting_segs, s2, 0);
#endif
    assert(s2->left == s1);
    assert(s1->right == s2);
    actlist_swap(status->actlist, s1, s2);
    assert(s2->right  == s1);
    assert(s1->left == s2);
    segment_t*left = s2->left;
    segment_t*right = s1->right;
    if(left)
        schedule_crossing(status, left, s2);
    if(right)
        schedule_crossing(status, s1, right);
}

typedef struct _box {
    point_t left1, left2, right1, right2;
} box_t;
static inline box_t box_new(int32_t x, int32_t y)
{
    box_t box;
    box.right1.x = box.right2.x = x;
    box.left1.x = box.left2.x = x-1;
    box.left1.y = box.right1.y = y-1;
    box.left2.y = box.right2.y = y;
    return box;
}

static void insert_point_into_segment(status_t*status, segment_t*s, point_t p)
{
    assert(s->pos.x != p.x || s->pos.y != p.y);

#ifdef CHECKS
    if(!dict_contains(status->segs_with_point, s))
        dict_put(status->segs_with_point, s, 0);
    assert(s->fs_out_ok);
#endif

    if(s->fs_out) {
#ifdef DEBUG
        fprintf(stderr, "[%d] receives next point (%d,%d)->(%d,%d) (drawing)\n", s->nr,
                s->pos.x, s->pos.y, p.x, p.y);
#endif
        // omit horizontal lines
        if(s->pos.y != p.y) {
            point_t a = s->pos;
            point_t b = p;
            assert(a.y != b.y);
	    status->writer.moveto(&status->writer, a.x, a.y);
	    status->writer.lineto(&status->writer, b.x, b.y);
        }
    } else {
#ifdef DEBUG
        fprintf(stderr, "[%d] receives next point (%d,%d) (omitting)\n", s->nr, p.x, p.y);
#endif
    }
    s->pos = p;
}

typedef struct _segrange {
    double xmin;
    segment_t*segmin;
    double xmax;
    segment_t*segmax;
} segrange_t;

static void segrange_adjust_endpoints(segrange_t*range, int32_t y)
{
#define XPOS_EQ(s1,s2,ypos) (XPOS((s1),(ypos))==XPOS((s2),(ypos)))
    segment_t*min = range->segmin;
    segment_t*max = range->segmax;

    /* we need this because if two segments intersect exactly on
       the scanline, segrange_test_segment_{min,max} can't tell which
       one is smaller/larger */
    if(min) while(min->left && XPOS_EQ(min, min->left, y)) {
        min = min->left;
    }
    if(max) while(max->right && XPOS_EQ(max, max->right, y)) {
        max = max->right;
    }
    range->segmin = min;
    range->segmax = max;
}
static void segrange_test_segment_min(segrange_t*range, segment_t*seg, int32_t y)
{
    if(!seg) return;
    /* we need to calculate the xpos anew (and can't use start coordinate or
       intersection coordinate), because we need the xpos exactly at the end of
       this scanline.
     */
    double x = XPOS(seg, y);
    if(!range->segmin || x<range->xmin) {
        range->segmin = seg;
        range->xmin = x;
    }
}
static void segrange_test_segment_max(segrange_t*range, segment_t*seg, int32_t y)
{
    if(!seg) return;
    double x = XPOS(seg, y);
    if(!range->segmax || x>range->xmax) {
        range->segmax = seg;
        range->xmax = x;
    }
}

/*
   SLOPE_POSITIVE:
      \+     \ +
------ I      \I
      -I\----  I
       I \   --I\---
       I  \    I \  -------
       +   \   +  \
*/
static void add_points_to_positively_sloped_segments(status_t*status, int32_t y, segrange_t*range)
{
    segment_t*first=0, *last = 0;
    int t;
    for(t=0;t<status->xrow->num;t++) {
        box_t box = box_new(status->xrow->x[t], y);
        segment_t*seg = actlist_find(status->actlist, box.left2, box.left2);

        seg = actlist_right(status->actlist, seg);
        while(seg) {
            if(seg->a.y == y) {
                // this segment started in this scanline, ignore it
                seg->changed = 1;last = seg;if(!first) {first=seg;}
            } else if(seg->delta.x <= 0) {
                // ignore segment w/ negative slope
            } else {
                last = seg;if(!first) {first=seg;}
                double d1 = LINE_EQ(box.right1, seg);
                double d2 = LINE_EQ(box.right2, seg);
                if(d1>0 || d2>=0) {
                    seg->changed = 1;
                    insert_point_into_segment(status, seg, box.right2);
                } else {
                    /* we unfortunately can't break here- the active list is sorted according
                       to the *bottom* of the scanline. hence pretty much everything that's still
                       coming might reach into our box */
                    //break;
                }
            }
            seg = seg->right;
        }
    }
    segrange_test_segment_min(range, first, y);
    segrange_test_segment_max(range, last, y);
}
/* SLOPE_NEGATIVE:
   |   +   /|  +  /    /
   |   I  / |  I /    /
   |   I /  |  I/    /
   |   I/   |  I    /
   |   I    | /I   /
   |  /+    |/ +  /
*/
static void add_points_to_negatively_sloped_segments(status_t*status, int32_t y, segrange_t*range)
{
    segment_t*first=0, *last = 0;
    int t;
    for(t=status->xrow->num-1;t>=0;t--) {
        box_t box = box_new(status->xrow->x[t], y);
        segment_t*seg = actlist_find(status->actlist, box.right2, box.right2);

        while(seg) {
            if(seg->a.y == y) {
                // this segment started in this scanline, ignore it
                seg->changed = 1;last = seg;if(!first) {first=seg;}
            } else if(seg->delta.x > 0) {
                // ignore segment w/ positive slope
            } else {
                last = seg;if(!first) {first=seg;}
                double d1 = LINE_EQ(box.left1, seg);
                double d2 = LINE_EQ(box.left2, seg);
                if(d1<0 || d2<0) {
                    seg->changed = 1;
                    insert_point_into_segment(status, seg, box.right2);
                } else {
                    //break;
                }
            }
            seg = seg->left;
        }
    }
    segrange_test_segment_min(range, last, y);
    segrange_test_segment_max(range, first, y);
}

/* segments ending in the current scanline need xrow treatment like everything else.
   (consider an intersection taking place just above a nearly horizontal segment
   ending on the current scanline- the intersection would snap down *below* the
   ending segment if we don't add the intersection point to the latter right away)
   we need to treat ending segments seperately, however. we have to delete them from
   the active list right away to make room for intersect operations (which might
   still be in the current scanline- consider two 45° polygons and a vertical polygon
   intersecting on an integer coordinate). but once they're no longer in the active list,
   we can't use the add_points_to_*_sloped_segments() functions anymore, and re-adding
   them to the active list just for point snapping would be overkill.
   (One other option to consider, however, would be to create a new active list only
    for ending segments)
*/
static void add_points_to_ending_segments(status_t*status, int32_t y)
{
    segment_t*seg = status->ending_segments;
    while(seg) {
        segment_t*next = seg->right;seg->right=0;

        assert(seg->b.y == status->y);

        if(status->xrow->num == 1) {
            // shortcut
	    assert(seg->b.x == status->xrow->x[0]);
            point_t p = {status->xrow->x[0], y};
            insert_point_into_segment(status, seg, p);
        } else {
            int t;
            int start=0,end=status->xrow->num,dir=1;
            if(seg->delta.x < 0) {
                start = status->xrow->num-1;
                end = dir = -1;
            }
            for(t=start;t!=end;t+=dir) {
                box_t box = box_new(status->xrow->x[t], y);
                double d0 = LINE_EQ(box.left1, seg);
                double d1 = LINE_EQ(box.left2, seg);
                double d2 = LINE_EQ(box.right1, seg);
                double d3 = LINE_EQ(box.right2, seg);
                if(!(d0>=0 && d1>=0 && d2>=0 && d3>0 ||
                     d0<=0 && d1<=0 && d2<=0 && d3<0)) {
                    insert_point_into_segment(status, seg, box.right2);
                    break;
                }
            }

#ifdef CHECKS
            /* we *need* to find a point to insert. the segment's own end point
               is in that list, for Pete's sake. */
            assert(t!=end);
#endif
        }
        // now that this is done, too, we can also finally free this segment
        segment_destroy(seg);
        seg = next;
    }
    status->ending_segments = 0;
}

static void recalculate_windings(status_t*status, segrange_t*range)
{
#ifdef DEBUG
    fprintf(stderr, "range: [%d]..[%d]\n", SEGNR(range->segmin), SEGNR(range->segmax));
#endif
    segrange_adjust_endpoints(range, status->y);

    segment_t*s = range->segmin;
    segment_t*end = range->segmax;
    segment_t*last = 0;

#ifdef DEBUG
    s = actlist_leftmost(status->actlist);
    while(s) {
        fprintf(stderr, "[%d]%d%s ", s->nr, s->changed,
            s == range->segmin?"S":(
            s == range->segmax?"E":""));
        s = s->right;
    }
    fprintf(stderr, "\n");
    s = range->segmin;
#endif
#ifdef CHECKS
    /* test sanity: check that we don't have changed segments
       outside of the given range */
    s = actlist_leftmost(status->actlist);
    while(s && s!=range->segmin) {
        assert(!s->changed);
        s = s->right;
    }
    s = actlist_rightmost(status->actlist);
    while(s && s!=range->segmax) {
        assert(!s->changed);
        s = s->left;
    }
    /* in check mode, go through the whole interval so we can test
       that all polygons where the fillstyle changed also have seg->changed=1 */
    s = actlist_leftmost(status->actlist);
    end = 0;
#endif

    if(end)
        end = end->right;
    while(s!=end) {
#ifndef CHECKS
        if(s->changed)
#endif
        {
            segment_t* left = actlist_left(status->actlist, s);
            windstate_t wind = left?left->wind:status->windrule->start(status->context);
            s->wind = status->windrule->add(status->context, wind, s->fs, s->dir, s->polygon_nr);
            fillstyle_t*fs_old = s->fs_out;
            s->fs_out = status->windrule->diff(&wind, &s->wind);

#ifdef DEBUG
            fprintf(stderr, "[%d] %s/%d/%s/%s %s\n", s->nr, s->dir==DIR_UP?"up":"down", s->wind.wind_nr, s->wind.is_filled?"fill":"nofill", s->fs_out?"draw":"omit",
		    fs_old!=s->fs_out?"CHANGED":"");
#endif
            assert(!(!s->changed && fs_old!=s->fs_out));
            s->changed = 0;

#ifdef CHECKS
            s->fs_out_ok = 1;
#endif
        }
        s = s->right;
    }
}

/* we need to handle horizontal lines in order to add points to segments
   we otherwise would miss during the windrule re-evaluation */
static void intersect_with_horizontal(status_t*status, segment_t*h)
{
    segment_t* left = actlist_find(status->actlist, h->a, h->a);
    segment_t* right = actlist_find(status->actlist, h->b, h->b);

    /* not strictly necessary, also done by the event */
    xrow_add(status->xrow, h->a.x);
    point_t o = h->a;

    if(!right) {
        assert(!left);
        return;
    }

    left = actlist_right(status->actlist, left); //first seg to the right of h->a
    right = right->right; //first seg to the right of h->b
    segment_t* s = left;

    while(s!=right) {
        assert(s);
        int32_t x = XPOS_INT(s, status->y);
#ifdef DEBUG
        fprintf(stderr, "...into [%d] (%d,%d) -> (%d,%d) at (%d,%d)\n", s->nr,
                s->a.x, s->a.y,
                s->b.x, s->b.y,
                x, status->y
                );
#endif
        assert(x >= h->a.x);
        assert(x <= h->b.x);
        assert(s->delta.x > 0 && x >= s->a.x || s->delta.x <= 0 && x <= s->a.x);
        assert(s->delta.x > 0 && x <= s->b.x || s->delta.x <= 0 && x >= s->b.x);
        xrow_add(status->xrow, x);

        s = s->right;
    }
}

static void event_apply(status_t*status, event_t*e)
{
    switch(e->type) {
        case EVENT_HORIZONTAL: {
            segment_t*s = e->s1;
#ifdef DEBUG
            event_dump(e);
#endif
            intersect_with_horizontal(status, s);
	    advance_stroke(status->queue, s->stroke, s->polygon_nr, s->stroke_pos);
            segment_destroy(s);e->s1=0;
            break;
        }
        case EVENT_END: {
            //delete segment from list
            segment_t*s = e->s1;
#ifdef DEBUG
            event_dump(e);
#endif
#ifdef CHECKS
            dict_del(status->intersecting_segs, s);
            dict_del(status->segs_with_point, s);
            assert(!dict_contains(status->intersecting_segs, s));
            assert(!dict_contains(status->segs_with_point, s));
#endif
            segment_t*left = s->left;
            segment_t*right = s->right;
            actlist_delete(status->actlist, s);
            if(left && right)
                schedule_crossing(status, left, right);

	    /* schedule segment for xrow handling */
            s->left = 0; s->right = status->ending_segments;
            status->ending_segments = s;
	    advance_stroke(status->queue, s->stroke, s->polygon_nr, s->stroke_pos);
            break;
        }
        case EVENT_START: {
            //insert segment into list
#ifdef DEBUG
            event_dump(e);
#endif
            segment_t*s = e->s1;
	    assert(e->p.x == s->a.x && e->p.y == s->a.y);
            actlist_insert(status->actlist, s->a, s->b, s);
            segment_t*left = s->left;
            segment_t*right = s->right;
            if(left)
                schedule_crossing(status, left, s);
            if(right)
                schedule_crossing(status, s, right);
            schedule_endpoint(status, s);
            break;
        }
        case EVENT_CROSS: {
            // exchange two segments
#ifdef DEBUG
            event_dump(e);
#endif
            if(e->s1->right == e->s2) {
		assert(e->s2->left == e->s1);
                exchange_two(status, e);
            } else {
		assert(e->s2->left != e->s1);
#ifdef DEBUG
		fprintf(stderr, "Ignore this crossing ([%d] not next to [%d])\n", e->s1->nr, e->s2->nr);
#endif
#ifdef REMEMBER_CROSSINGS
                /* ignore this crossing for now (there are some line segments in between).
                   it'll get rescheduled as soon as the "obstacles" are gone */
                char del1 = dict_del(&e->s1->scheduled_crossings, (void*)(ptroff_t)e->s2->nr);
                char del2 = dict_del(&e->s2->scheduled_crossings, (void*)(ptroff_t)e->s1->nr);
                assert(del1 && del2);
#endif
#ifdef CHECKS
                point_t pair;
                pair.x = e->s1->nr;
                pair.y = e->s2->nr;
#ifdef REMEMBER_CROSSINGS
                assert(dict_contains(status->seen_crossings, &pair));
                dict_del(status->seen_crossings, &pair);
#endif
#endif
            }
        }
    }
}

#ifdef CHECKS
static void check_status(status_t*status)
{
    DICT_ITERATE_KEY(status->intersecting_segs, segment_t*, s) {
        if((s->pos.x != s->b.x ||
            s->pos.y != s->b.y) &&
           !dict_contains(status->segs_with_point, s)) {
            fprintf(stderr, "Error: segment [%d] (%sslope) intersects in scanline %d, but it didn't receive a point\n",
                    s->nr,
                    s->delta.x<0?"-":"+",
                    status->y);
            assert(0);
        }
    }
}
#endif

static void add_horizontals(gfxpoly_t*poly, windrule_t*windrule, windcontext_t*context)
{
    /*
          |..|        |...........|                 |           |
          |..|        |...........|                 |           |
          |..+        +        +..|                 +--+     +--+
          |...........|        |..|                    |     |
          |...........|        |..|                    |     |
     */

#ifdef DEBUG
    fprintf(stderr, "========================================================================\n");
#endif
    heap_t* queue = heap_new(sizeof(event_t), compare_events_simple);
    gfxpoly_enqueue(poly, queue, 0);

    actlist_t* actlist = actlist_new();

    event_t*e = heap_chopmax(queue);
    while(e) {
        int32_t y = e->p.y;
        int32_t x = 0;
        char fill = 0;
#ifdef DEBUG
        fprintf(stderr, "----------------------------------- %d\n", y);
        actlist_dump(actlist, y-1);
#endif
#ifdef CHECKS
        actlist_verify(actlist, y-1);
#endif
        do {
            if(fill && x != e->p.x) {
#ifdef DEBUG
                fprintf(stderr, "%d) draw horizontal line from %d to %d\n", y, x, e->p.x);
#endif
		assert(x<e->p.x);

                gfxpolystroke_t*stroke = rfx_calloc(sizeof(gfxpolystroke_t));
		stroke->next = poly->strokes;
		poly->strokes = stroke;

		stroke->num_points = 2;
		stroke->points = malloc(sizeof(point_t)*2);
		stroke->dir = DIR_UP; // FIXME
		stroke->fs = 0;
		point_t a,b;
                a.y = b.y = y;
		/* we draw from low x to high x so that left/right fillstyles add up
                   (because the horizontal line's fill style controls the area *below* the line)
		 */
                a.x = e->p.x;
                b.x = x;
		stroke->points[0] = a;
		stroke->points[1] = b;
#ifdef CHECKS
		/* the output should always be intersection free polygons, so check this horizontal
		   line isn't hacking through any segments in the active list */
		segment_t* start = actlist_find(actlist, b, b);
		segment_t* s = actlist_find(actlist, a, a);
		while(s!=start) {
		    assert(s->a.y == y || s->b.y == y);
		    s = s->left;
		}
#endif
            }
            segment_t*left = 0;
            segment_t*s = e->s1;

            switch(e->type) {
                case EVENT_START: {
		    assert(e->p.x == s->a.x && e->p.y == s->a.y);
                    actlist_insert(actlist, s->a, s->b, s);
                    event_t e;
                    e.type = EVENT_END;
                    e.p = s->b;
                    e.s1 = s;
                    e.s2 = 0;
                    heap_put(queue, &e);
                    left = actlist_left(actlist, s);
                    break;
                }
                case EVENT_END: {
                    left = actlist_left(actlist, s);
                    actlist_delete(actlist, s);
		    advance_stroke(queue, s->stroke, s->polygon_nr, s->stroke_pos);
                    break;
                }
                default: assert(0);
            }

            x = e->p.x;
            fill ^= 1;//(before.is_filled != after.is_filled);
#ifdef DEBUG
            fprintf(stderr, "%d) event=%s[%d] left:[%d] x:%d\n",
                    y, e->type==EVENT_START?"start":"end",
                    s->nr,
                    left?left->nr:-1,
                    x);
#endif

            if(e->type == EVENT_END)
                segment_destroy(s);

            free(e);
            e = heap_chopmax(queue);
        } while(e && y == e->p.y);

        assert(!fill); // check that polygon is not bleeding
    }

    actlist_destroy(actlist);
    heap_destroy(queue);
}

gfxpoly_t* gfxpoly_process(gfxpoly_t*poly, windrule_t*windrule, windcontext_t*context)
{
    current_polygon = poly;
    heap_t* queue = heap_new(sizeof(event_t), compare_events);

    gfxpoly_enqueue(poly, queue, /*polygon nr*/0);

    status_t status;
    memset(&status, 0, sizeof(status_t));
    status.queue = queue;
    status.windrule = windrule;
    status.context = context;
    status.actlist = actlist_new();
    gfxpolywriter_init(&status.writer);
    status.writer.setgridsize(&status.writer, poly->gridsize);

#ifdef CHECKS
    status.seen_crossings = dict_new2(&point_type);
    int lasty=heap_peek(queue)?((event_t*)heap_peek(queue))->p.y-1:0;
#endif

    status.xrow = xrow_new();

    event_t*e = heap_chopmax(queue);
    while(e) {
        status.y = e->p.y;
	assert(status.y>=lasty);
#ifdef CHECKS
        status.intersecting_segs = dict_new2(&ptr_type);
        status.segs_with_point = dict_new2(&ptr_type);
#endif

#ifdef DEBUG
        fprintf(stderr, "----------------------------------- %d\n", status.y);
        actlist_dump(status.actlist, status.y-1);
#endif
#ifdef CHECKS
        actlist_verify(status.actlist, status.y-1);
#endif
        xrow_reset(status.xrow);
        do {
            xrow_add(status.xrow, e->p.x);
            event_apply(&status, e);
            free(e);
            e = heap_chopmax(queue);
        } while(e && status.y == e->p.y);

        xrow_sort(status.xrow);
        segrange_t range;
        memset(&range, 0, sizeof(range));
#ifdef DEBUG
        actlist_dump(status.actlist, status.y);
#endif
        add_points_to_positively_sloped_segments(&status, status.y, &range);
        add_points_to_negatively_sloped_segments(&status, status.y, &range);
        add_points_to_ending_segments(&status, status.y);

        recalculate_windings(&status, &range);
#ifdef CHECKS
        check_status(&status);
        dict_destroy(status.intersecting_segs);
        dict_destroy(status.segs_with_point);
#endif
    }
#ifdef CHECKS
    dict_destroy(status.seen_crossings);
#endif
    actlist_destroy(status.actlist);
    heap_destroy(queue);
    xrow_destroy(status.xrow);

    gfxpoly_t*p = (gfxpoly_t*)status.writer.finish(&status.writer);

    add_horizontals(p, &windrule_evenodd, context); // output is always even/odd
    return p;
}