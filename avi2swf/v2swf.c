/*  v2swf.c 
    part of swftools

    Copyright (C) 2003 Matthias Kramm <kramm@quiss.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include "v2swf.h"
#include "../lib/rfxswf.h"
#include "ringbuffer.c"

typedef struct _v2swf_internal_t
{
    TAG*tag;

    int filesize;
    int headersize;
    int frames;
    
    int myframes;

    struct writer_t out;
    struct writer_t out2;

    ringbuffer_t r;
    videoreader_t* video;

    int width;
    int height;

    unsigned char* vrbuffer;
    unsigned char* buffer;
    unsigned char* lastbitmap;

    int id;
    int lastid;

    int quality;
    int blockdiff;
    int keyframe_interval;
    int diffmode;

    float framerate;
    float fpsratio;
    float fpspos;

    int bitrate;

    int finished;
    int keyframe;
    int showframe;

    float samplepos;
    float framesamplepos;
    int samplewritepos;
    double soundframepos;
    int soundstreamhead;
    int seek;

    double audio_fix;
    int fixheader;
    int prescale;

    int scale;
    
    int domotion;

    int head_done;

    int version;

    VIDEOSTREAM stream;

} v2swf_internal_t;

static int verbose = 0;
static int filelog = 0;

static void logf(char*format, ...)
{
    char buf[1024];
    int l;
    va_list arglist;
    if(!verbose)
	return;
    va_start(arglist, format);
    vsprintf(buf, format, arglist);
    va_end(arglist);
    l = strlen(buf);
    while(l && buf[l-1]=='\n') {
	buf[l-1] = 0;
	l--;
    }
    if(filelog)
    {
	FILE*fi = fopen("debug.log", "ab+");
	fprintf(fi, "(v2swf) %s\n", buf);
    	fflush(fi);
    	fclose(fi);
    }

    printf("(v2swf) %s\n", buf);
    fflush(stdout);
}

static void writeShape(v2swf_internal_t*i, int id, int gfxid, int width, int height)
{
    RGBA rgb;
    MATRIX m;
    SHAPE*shape;
    SRECT r;
    int lines = 0;
    int ls,fs;
    swf_ResetTag(i->tag, ST_DEFINESHAPE);
    swf_ShapeNew(&shape);
    rgb.b = rgb.g = rgb.r = 0xff;
    if(lines)
	ls = swf_ShapeAddLineStyle(shape,20,&rgb);  
    swf_GetMatrix(NULL,&m);
    m.sx = 20*65536;
    m.sy = 20*65536;

    fs = swf_ShapeAddBitmapFillStyle(shape,&m,gfxid,0);
    swf_SetU16(i->tag,id);   // ID   
    r.xmin = 0;
    r.ymin = 0;
    r.xmax = width*20;
    r.ymax = height*20;
    swf_SetRect(i->tag,&r);

    swf_SetShapeStyles(i->tag,shape);
    swf_ShapeCountBits(shape,NULL,NULL);
    swf_SetShapeBits(i->tag,shape);

    swf_ShapeSetAll(i->tag,shape,0,0,lines?ls:0,fs,0);

    swf_ShapeSetLine(i->tag,shape,width*20,0);
    swf_ShapeSetLine(i->tag,shape,0,height*20);
    swf_ShapeSetLine(i->tag,shape,-width*20,0);
    swf_ShapeSetLine(i->tag,shape,0,-height*20);
    swf_ShapeSetEnd(i->tag);
    i->filesize += swf_WriteTag2(&i->out, i->tag);
    swf_ShapeFree(shape);
}

static int getSamples(videoreader_t*video, S16*data, int len, double speedup)
{
    double pos = 0;
    double ratio = video->rate * speedup / 44100.0;
    int rlen = (int)(len * ratio);
    int t;
    S16 tmp[576*32];
    int r = /*resampled len */ rlen * 
	          /* s16_le */ 2 * 
		               video->channels;
    if(videoreader_getsamples(video, tmp, r) < r)
	return 0;

    /* convert to 1 channel */
    for(t=0;t<rlen;t++) {
	int s;
	int a=0;
	for(s=0;s<video->channels;s++)
	    a += tmp[t*video->channels+s];
	tmp[t] = a/video->channels;
    }

    /* down/up-sample to 44khz */
    for(t=0;t<len;t++) {
	data[t] = tmp[(int)pos];
	pos+=ratio;
    }
    return 1;
}

extern int swf_mp3_channels;
extern int swf_mp3_bitrate;
extern int swf_mp3_samplerate;
static void writeAudioForOneFrame(v2swf_internal_t* i)
{
    int blocksize; 
    double blockspersecond;
    double framespersecond, framesperblock, samplesperframe, samplesperblock;
    int seek;
    int s;
    double speedup = i->audio_fix;
    int num = 0;
    int pos = 0;
    S16 block1[576*4 * 2];

    logf("writeAudioForOneFrame()");

    if(i->video->channels<=0 || i->video->rate<=0)
	return; /* no sound in video */

    blocksize = 576; /* 11khz samples per mp3 block */
    blockspersecond = 11025.0/blocksize;

    /* notice: for framerates greater than about 35, audio starts getting choppy. */
    framespersecond = i->framerate;

    framesperblock = framespersecond / blockspersecond;
    samplesperframe = (blocksize * blockspersecond) / framespersecond; /* 11khz-samples per frame */
    samplesperblock = samplesperframe * framesperblock;

    logf("samplesperblock: %f", samplesperblock);

    if(!i->soundstreamhead) {
	/* first run - initialize */
	swf_mp3_channels = 1;//i->video->channels;
	swf_mp3_bitrate = i->bitrate;
	swf_ResetTag(i->tag, ST_SOUNDSTREAMHEAD);
	/* samplesperframe overrides the movie framerate: */
	logf("swf_SetSoundStreamHead(): %08x %d", i->tag, samplesperframe);
	swf_SetSoundStreamHead(i->tag, samplesperframe);
	logf("swf_SetSoundStreamHead() done");
	i->filesize += swf_WriteTag2(&i->out, i->tag);
	i->soundstreamhead = 1;
    }

    /* for framerates greater than 19.14, every now and then a frame
       hasn't a soundstreamblock. Determine whether this is the case.
    */
    if(i->frames < i->soundframepos) {
	i->samplepos += samplesperframe;
	return;
    }

    seek = i->seek;

    //while(i->samplewritepos + num * blocksize < i->samplepos + blocksize) {
    while(i->samplewritepos < i->samplepos + blocksize) {
	i->samplewritepos += blocksize;
	i->soundframepos += framesperblock;
	num++;
    }
    if(!num) {
	logf("num is zero");
	printf(" num is zero\n");
	fprintf(stderr, "  num is zero\n");
    }
    logf("num: %d", num);

    /* write num frames, max 1 block */
    for(pos=0;pos<num;pos++) {
	logf("pos: %d- getsamples", pos);
	if(!getSamples(i->video, block1, 576*4, speedup)) { /* 4 = 44100/11025 */
	    i->video->rate = i->video->channels = 0; //end of soundtrack
	    return;
	}
	logf("pos: %d- encode mp3", pos);
	if(!pos) {
	    swf_ResetTag(i->tag, ST_SOUNDSTREAMBLOCK);
	    swf_SetSoundStreamBlock(i->tag, block1, seek, num);
	} else {
	    swf_SetSoundStreamBlock(i->tag, block1, seek, 0);
	}
    }
    i->filesize += swf_WriteTag2(&i->out, i->tag);

    i->seek = i->samplewritepos - (i->samplepos + blocksize);
    i->samplepos += samplesperframe;
    logf("writeSamplesForOneFrame(): done");
}

static void writeShowFrame(v2swf_internal_t* i)
{
    do {
	writeAudioForOneFrame(i);
	
	swf_ResetTag(i->tag, ST_SHOWFRAME);
	i->filesize += swf_WriteTag2(&i->out, i->tag);

	i->fpspos -= 1.0;
	i->frames ++;
    }
    while(i->fpspos >= 1.0);
    i->showframe = 0;
}

static void writeShowTags(v2swf_internal_t* i, int shapeid, int bmid, int width, int height)
{
    writeShape(i, shapeid, bmid, width, height);

    swf_ResetTag(i->tag, ST_PLACEOBJECT2);
    if(!i->prescale) {
	MATRIX m;
	swf_GetMatrix(0, &m);
	m.sx = m.sy = i->scale;
	swf_ObjectPlace(i->tag,shapeid,shapeid,&m,0,0);
    } else {
	swf_ObjectPlace(i->tag,shapeid,shapeid,0,0,0);
    }
    i->filesize += swf_WriteTag2(&i->out, i->tag);

    i->showframe = 1;
}

static int wwrite(struct writer_t*w, void*data, int len)
{
    v2swf_internal_t* i = (v2swf_internal_t*)w->internal;
    ringbuffer_put(&i->r, data, len);
    return len;
}

static void wfinish(struct writer_t*w)
{
    v2swf_internal_t* i = (v2swf_internal_t*)w->internal;
}

static void writehead(v2swf_internal_t*i)
{
    char header[]="FWS\6\0\0\0\4";
    SWF swf;
    int ret;
    int id;
   
    header[3] = i->version;
    if(i->version >= 6) { //MX
	header[0] = 'C';

	i->out2.write = wwrite;
	i->out2.finish = wfinish;
	i->out2.internal = i;
	i->out2.type = 77;
	i->out2.bitpos = 0;
	i->out2.mybyte = 0;
	i->out2.pos = 0;
	writer_init_zlibdeflate(&i->out, &i->out2);
    } else {
	i->out.write = wwrite;
	i->out.finish = wfinish;
	i->out.internal = i;
	i->out.type = 77;
	i->out.bitpos = 0;
	i->out.mybyte = 0;
	i->out.pos = 0;
	i->out2 = i->out;
    }

    if(i->prescale) {
	i->width = (int)(i->video->width*(i->scale/65536.0));
	i->height = (int)(i->video->height*(i->scale/65536.0));
    } else {
	i->width = i->video->width;
	i->height = i->video->height;
    }
    if(!i->width)
	i->width = 1;
    if(!i->height)
	i->height = 1;
    i->buffer = (unsigned char*)malloc(i->width*i->height*4);
    i->vrbuffer = (unsigned char*)malloc(i->video->width*i->video->height*4);

    memset(&swf, 0, sizeof(SWF));
    swf.fileVersion=i->version;
    swf.fileSize = 0;
    swf.frameCount = 65535;
    swf.movieSize.xmax=i->width*20;
    swf.movieSize.ymax=i->height*20;
    swf.compressed = 8; /* 8 = compression done by caller (us) */
    swf.frameRate = (int)(i->framerate*0x100);//25*0x100;

    /* write the first 8 bytes to out */
    i->out2.write(&i->out2, header, 8);

    i->filesize += swf_WriteHeader2(&i->out, &swf);
    i->headersize = i->filesize;
    
    i->tag = swf_InsertTag(NULL,  ST_SETBACKGROUNDCOLOR);
    swf_SetU8(i->tag, 0); //black
    swf_SetU8(i->tag, 0);
    swf_SetU8(i->tag, 0);
    i->filesize += swf_WriteTag2(&i->out, i->tag);
}

static void finish(v2swf_internal_t*i)
{
    logf("finish(): i->finished=%d\n", i->finished);
    if(!i->finished) {
	logf("write endtag\n", i->finished);

	swf_ResetTag(i->tag, ST_END);
	i->filesize += swf_WriteTag2(&i->out, i->tag);
	i->out.finish(&i->out);

	if(i->version>=6) {
	    swf_VideoStreamClear(&i->stream);
	}
	if(i->buffer)  {
	    free(i->buffer);i->buffer = 0;
	}
	if(i->vrbuffer)  {
	    free(i->vrbuffer);i->vrbuffer = 0;
	}
	if(i->lastbitmap)  {
	    free(i->lastbitmap);i->lastbitmap = 0;
	}

	/* FIXME: we shouldn't be doing this. the caller should */
	logf("call videoreader_close(%08x)\n", i->video);
	videoreader_close(i->video);

	i->finished = 1;
    }
    logf("finishing done\n");
}
static void cleanup(v2swf_internal_t*i)
{
    int t;
    for(t=i->lastid;t<i->id;t++) {
	if(!(t&1)) {
	    swf_ResetTag(i->tag, ST_REMOVEOBJECT2);
	    swf_SetU16(i->tag, t);
	    i->filesize += swf_WriteTag2(&i->out, i->tag);
	}
	swf_ResetTag(i->tag, ST_FREECHARACTER);
	swf_SetU16(i->tag, t);
	i->filesize += swf_WriteTag2(&i->out, i->tag);
    }
    i->lastid = i->id;
}

#define DIFFMODE_MAX 1 
#define DIFFMODE_MEAN 2
#define DIFFMODE_EXACT 3
#define DIFFMODE_QMEAN 4

static int blockdiff_max(U8*d1,U8*d2,int yadd, int maxdiff, int xl, int yl)
{
    int x,y;
    for(y=0;y<yl;y++) {
	for(x=0;x<xl;x++) {
	    int rd = d1[1] - d2[1];
	    int gd = d1[2] - d2[2];
	    int bd = d1[3] - d2[3];
	    if(rd < 0) rd = -rd;
	    if(gd < 0) gd = -gd;
	    if(bd < 0) bd = -bd;
	    if(rd+gd+bd>maxdiff)
		return 1;

	    d1+=4; d2+=4;
	}
	d1 += yadd; d2 += yadd;
    }
    return 0;
}

static int blockdiff_mean(U8*d1,U8*d2,int yadd, int maxdiff, int xl, int yl)
{
    int mean = 0;
    int x,y;
    for(y=0;y<yl;y++) {
	for(x=0;x<xl;x++) {
	    int rd = d1[1] - d2[1];
	    int gd = d1[2] - d2[2];
	    int bd = d1[3] - d2[3];
	    if(rd < 0) rd = -rd;
	    if(gd < 0) gd = -gd;
	    if(bd < 0) bd = -bd;
	    mean += rd+gd+bd;

	    d1+=4; d2+=4;
	}
	d1 += yadd; d2 += yadd;
    }
    if(mean/(xl*yl) > maxdiff)
	return 1;
    return 0;
}

static int blockdiff_qmean(U8*d1,U8*d2,int yadd, int maxdiff, int xl, int yl)
{
    int mean = 0;
    int x,y;
    for(y=0;y<yl;y++) {
	for(x=0;x<xl;x++) {
	    int rd = d1[1] - d2[1];
	    int gd = d1[2] - d2[2];
	    int bd = d1[3] - d2[3];
	    int q;
	    if(rd < 0) rd = -rd;
	    if(gd < 0) gd = -gd;
	    if(bd < 0) bd = -bd;
	    q = rd+gd+bd;
	    mean += q*q;

	    d1+=4; d2+=4;
	}
	d1 += yadd; d2 += yadd;
    }
    if(mean/(xl*yl) > maxdiff*maxdiff)
	return 1;
    return 0;
}

static int blockdiff_exact(U8*d1,U8*d2,int yadd, int xl, int yl)
{
    int x,y;
    for(y=0;y<yl;y++) {
	for(x=0;x<xl;x++) {
	    if((*(U32*)d1^*(U32*)d2)&0xffffff00) { //bits [RGB_] of [RGBA] differ
		return 1;
	    }
	    d1+=4; d2+=4;
	}
	d1 += yadd; d2 += yadd;
    }
    return 0;
}

		/*U32 r = (*(U32*)d1^-(U32*)d2)&0xffffff00;
		U32 g = ((r << 3) ^ r)&0x80808080;
		if(g) 
		    goto differ;*/

static void checkInit(v2swf_internal_t*i)
{
    if(!i->head_done) {
	writehead(i);
	if(i->version>=6) {
	    swf_ResetTag(i->tag,  ST_DEFINEVIDEOSTREAM);
	    swf_SetU16(i->tag, 99);
	    swf_SetVideoStreamDefine(i->tag, &i->stream, 65535, i->width, i->height);
	    i->filesize += swf_WriteTag2(&i->out, i->tag);
	    if(i->domotion) {
		i->stream.do_motion = 1;
	    }
	}
	i->head_done = 1;
    }
}

static void scaleimage(v2swf_internal_t*i)
{
    int x,y;
    int xv,yv;
    int xm = (i->video->width*65536)/i->width;
    int ym = (i->video->height*65536)/i->height;
    memset(i->buffer, 255, i->width*i->height*4);
    for(y=0,yv=0;y<i->height;y++,yv+=ym) {
	int*src = &((int*)i->vrbuffer)[(yv>>16)*i->video->width];
	int*dest = &((int*)i->buffer)[y*i->width];
	for(x=0,xv=0;x<i->width;x++,xv+=xm) {
	    dest[x] = src[xv>>16];
	}
    }
    //memcpy(i->buffer, i->vrbuffer, i->width*i->height*4);
}

static int encodeoneframe(v2swf_internal_t*i)
{
    videoreader_t*video = i->video;
    int ret;

    checkInit(i);

    if(videoreader_eof(i->video) || !videoreader_getimage(i->video, i->vrbuffer)) 
    {
	logf("videoreader returned eof\n");
	finish(i);
	return 0;
    }

    i->fpspos += i->fpsratio;

    /* skip frames */
    if(i->fpspos<1.0) {
	return 0;
    }
    
    logf("encoding image for frame %d\n", i->frames);

    if(i->showframe)
	writeShowFrame(i);

    logf("scaling\n");

    scaleimage(i);

    logf("version is %d\n", i->version);

    if(i->version <= 4) {

	int bmid = i->id++;
	int shapeid = i->id++;
	int width2 = i->width * 4;

	if(i->id>=4) {
	    swf_ResetTag(i->tag, ST_REMOVEOBJECT2);
	    swf_SetU16(i->tag, i->id-3);
	    i->filesize += swf_WriteTag2(&i->out, i->tag);
	    swf_ResetTag(i->tag, ST_FREECHARACTER);
	    swf_SetU16(i->tag, i->id-4);
	    i->filesize += swf_WriteTag2(&i->out, i->tag);
	}

	swf_ResetTag(i->tag, ST_DEFINEBITSJPEG2);
	swf_SetU16(i->tag, bmid);
	swf_SetJPEGBits2(i->tag, i->width, i->height, (RGBA*)i->buffer, i->quality);
	i->filesize += swf_WriteTag2(&i->out, i->tag);
	
	writeShowTags(i, shapeid, bmid, i->width, i->height);

    } else if(i->version == 5) {
	int width2 = i->width * 4;
	int width8 = (i->width+7)/8;
	int height8 = (i->height+7)/8;

	/* the idea is here to only update those jpeg 8x8 blocks
	   which actually have changed. This means that we have to keep
	   the bitmap from the last frame for the comparison. */

	(i->keyframe)--;
	if(!i->lastbitmap || !i->keyframe) {
	    int t, bmid,shapeid;
	    cleanup(i);

	    if(!i->lastbitmap) {
		logf("Creating bitmap buffer for %dx%d (%dx%d), (%dx%d)\n", i->width, i->height, width2, i->height, width8, height8);
		i->lastbitmap = (U8*)malloc(width2*i->height);
	    }
	    memcpy(i->lastbitmap, i->buffer, width2*i->height);

	    i->keyframe = i->keyframe_interval;

	    bmid = i->id++;
	    shapeid = i->id++;
	    width2 = i->width * 4;
	    swf_ResetTag(i->tag, ST_DEFINEBITSJPEG2);
	    swf_SetU16(i->tag, bmid);
	    swf_SetJPEGBits2(i->tag, i->width, i->height, (RGBA*)i->buffer, i->quality);
	    i->filesize += swf_WriteTag2(&i->out, i->tag);
	   
	    writeShowTags(i, shapeid, bmid, i->width, i->height);
	    return 1;
	} else {
	    /* The following looks so ugly because it's somewhat optimized. 
	       What it does is walk through all the 8x8 blocks, find those
	       which have changed too much and set all others to (R,G,B,A)=(0,0,0,0). 
	       It also set's alpha to 255 in those who haven't changed, and
	       copies them to lastbitmap.
	     */

	    int x8, y8;
	    //int maxdiff = ((100 - i->quality)*256)/100;
	    int maxdiff = i->blockdiff*3;
	    for(y8=0;y8<height8;y8++)
	    for(x8=0;x8<width8;x8++) {
		int x,y;
		int xl=8,yl=8;
		int yadd;
		U8*d1,*d1b,*d2,*d2b;
		if(x8*8+xl > i->width)
		    xl = i->width - x8*8;
		if(y8*8+yl > i->height)
		    yl = i->height - y8*8;
		d1 = &i->buffer[width2*y8*8+x8*8*4];
		d1b = d1;
		d2 = &i->lastbitmap[width2*y8*8+x8*8*4];
		d2b = d2;
		yadd = width2 - (xl*4);

		if(i->diffmode == DIFFMODE_MAX) {
		    if(blockdiff_max(d1, d2, yadd, maxdiff, xl, yl))
			goto differ;
		} else if(i->diffmode == DIFFMODE_MEAN) {
		    if(blockdiff_mean(d1, d2, yadd, maxdiff, xl, yl))
			goto differ;
		} else if(i->diffmode == DIFFMODE_EXACT) {
		    if(blockdiff_exact(d1, d2, yadd, xl, yl))
			goto differ;
		} else if(i->diffmode == DIFFMODE_QMEAN) {
		    if(blockdiff_qmean(d1, d2, yadd, maxdiff, xl, yl))
			goto differ;
		}

		for(y=0;y<yl;y++) {
		    for(x=0;x<xl;x++) {
			*(U32*)d1b = 0;
			d1b+=4;
		    }
		    d1b += yadd;
		}
		continue;
    differ:
		for(y=0;y<yl;y++) {
		    for(x=0;x<xl;x++) {
			*(U32*)d2b = *(U32*)d1b;
			d1b[0] = 255;
			d1b+=4;d2b+=4;
		    }
		    d1b += yadd; d2b += yadd;
		}
	    }

	    /* ok, done. Now a) data is zeroed out in regions which haven't changed
			     b) lastbitmap equals the bitmap we were called with
			     c) data's alpha value is set to 255 in regions which did change */

	}

	{
	    int bmid = i->id++;
	    int shapeid = i->id++;

	    swf_ResetTag(i->tag, ST_DEFINEBITSJPEG3);
	    swf_SetU16(i->tag, bmid);
	    swf_SetJPEGBits3(i->tag, i->width, i->height, (RGBA*)i->buffer, i->quality);
	    i->filesize += swf_WriteTag2(&i->out, i->tag);

	    writeShowTags(i, shapeid, bmid, i->width, i->height);
	}
    } else {
	int quant = 1+(30-(30*i->quality)/100);
	SWFPLACEOBJECT obj;

	swf_GetPlaceObject(0, &obj);
	if(!i->prescale) {
	    obj.matrix.sx = obj.matrix.sy = i->scale;
	}

	if(i->stream.frame==0) {
	    obj.depth = 1;
	    obj.id = 99;
	} else {
	    obj.move = 1;
	    obj.depth = 1;
	    obj.ratio = i->stream.frame;
	}

	swf_ResetTag(i->tag, ST_VIDEOFRAME);
	swf_SetU16(i->tag, 99);
	if(!(--i->keyframe)) {
	    logf("setting video I-frame, ratio=%d\n", i->stream.frame);
	    swf_SetVideoStreamIFrame(i->tag, &i->stream, (RGBA*)i->buffer, quant);
	    i->keyframe = i->keyframe_interval;
	} else {
	    logf("setting video P-frame, ratio=%d\n", i->stream.frame);
	    swf_SetVideoStreamPFrame(i->tag, &i->stream, (RGBA*)i->buffer, quant);
	}
	i->filesize += swf_WriteTag2(&i->out, i->tag);

	swf_ResetTag(i->tag, ST_PLACEOBJECT2);
	swf_SetPlaceObject(i->tag,&obj);
	i->filesize += swf_WriteTag2(&i->out, i->tag);
	i->showframe = 1;
    }
    return 1;
}

int v2swf_init(v2swf_t*v2swf, videoreader_t * video)
{
    int ret = 0;
    int t=0;
    v2swf_internal_t* i;
    logf("v2swf_init()\n");
    memset(v2swf, 0, sizeof(v2swf_t));
    i = (v2swf_internal_t*)malloc(sizeof(v2swf_internal_t));
    memset(i, 0, sizeof(v2swf_internal_t));
    v2swf->internal = i;

    ringbuffer_init(&i->r);

    logf("video: %dx%d, fps %f\n", video->width, video->height, video->fps);

    i->video = video;
    i->blockdiff = 64;
    i->keyframe_interval = 8;
    i->quality = 20;
    i->scale = 65536;
    i->prescale = 0;
    i->head_done = 0;
    i->diffmode = DIFFMODE_QMEAN;
    i->audio_fix = 1.0;
    i->fixheader = 0;
    i->framerate = video->fps;
    i->fpsratio = 1.00000000;
    i->fpspos = 0.0;
    i->bitrate = 32;
    i->version = 6;
    i->buffer = 0;
    i->lastbitmap = 0;
    i->filesize = 8;
    i->frames = 0;
    i->id = 1;
    i->lastid = 1;
    i->keyframe = 1;
    i->showframe = 0;

    memset(&i->out, 0, sizeof(struct writer_t));
    memset(&i->out2, 0, sizeof(struct writer_t));

    return 0;
}
int v2swf_read(v2swf_t*v2swf, void*buffer, int len)
{
    v2swf_internal_t* i;
    int l;
    logf("v2swf_read(%d)\n", len);
    i = (v2swf_internal_t*)v2swf->internal;

    while(!i->finished && i->r.available < len) {
	if(!encodeoneframe(i)) {
	    break;
	}
    }
    logf("v2swf_read() done: %d bytes available in ringbuffer\n", i->r.available);
    l = ringbuffer_read(&i->r, buffer, len);

    return l;
}
void v2swf_close(v2swf_t*v2swf)
{
    v2swf_internal_t* i = (v2swf_internal_t*)v2swf->internal;
    logf("close(): i->finished=%d\n", i->finished);

    /* needed only if aborting: */
    finish(i);

    logf("freeing memory\n");
    free(v2swf->internal);
    memset(v2swf, 0, sizeof(v2swf_t));
    logf("close() done\n");
}

static int mp3_bitrates[] =
{ 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};

void v2swf_setparameter(v2swf_t*v2swf, char*name, char*value)
{
    v2swf_internal_t* i;

    logf("set parameters %s to %s\n", name, value);

    if(!strcmp(name, "verbose")) {
	verbose = 1;
	logf("set parameters %s to %s\n", name, value);
	return;
    }

    if(!v2swf || !v2swf->internal) {
	printf("error- couldn't set parameter %s: not initialized yet\n", name);fflush(stdout);
	return;
    }
    i = (v2swf_internal_t*)v2swf->internal;

    if(!strcmp(name, "flash_version")) {
	i->version = atoi(value);
    } else if(!strcmp(name, "audiosync")) {
	i->audio_fix = (int)(atof(value));
    } else if(!strcmp(name, "scale")) {
	i->scale = (int)(atof(value)*65536);
    } else if(!strcmp(name, "scale65536")) {
	i->scale = atoi(value);
    } else if(!strcmp(name, "quality")) {
	i->quality = atoi(value);
    } else if(!strcmp(name, "motioncompensation")) {
	i->domotion = atoi(value);
    } else if(!strcmp(name, "prescale")) {
	i->prescale = atoi(value);
    } else if(!strcmp(name, "blockdiff")) {
	i->blockdiff = atoi(value);
    } else if(!strcmp(name, "fixheader")) {
	i->fixheader = atoi(value);
    } else if(!strcmp(name, "framerate")) {
	i->framerate = atof(value);
	i->fpsratio = i->framerate / i->video->fps;
    }
    else if(!strcmp(name, "mp3_bitrate")) {
	int t=0,o;
	i->bitrate = o = atoi(value);
	if(i->bitrate>160)
	    i->bitrate = 160;
	while(mp3_bitrates[t]) {
	    if(i->bitrate <= mp3_bitrates[t]) {
		i->bitrate = mp3_bitrates[t];
		break;
	    }
	    t++;
	}
	logf("bitrate %d requested, setting to %d", o, i->bitrate);
    }
    else if(!strcmp(name, "blockdiff_mode")) {
	if(!strcmp(value, "max")) i->diffmode = DIFFMODE_MAX;
	else if(!strcmp(value, "mean")) i->diffmode = DIFFMODE_MEAN;
	else if(!strcmp(value, "qmean")) i->diffmode = DIFFMODE_QMEAN;
	else if(!strcmp(value, "exact")) i->diffmode = DIFFMODE_EXACT;
	else {
	    printf("diffmode %s not recognized\n", value);
	    printf("valid diffmodes are: %s\n", "max, mean, qmean, exact");
	}
    }
    else if(!strcmp(name, "keyframe_interval")
            || !strcmp(name, "keyframe")) {
	int k = atoi(value);if(k<=0) k=1;
	i->keyframe_interval = k;
    }
    else {
	printf("Setting encoder.%s not recognized!\n", name);fflush(stdout);
	return;
    }
}
void v2swf_backpatch(v2swf_t*v2swf, char*filename)
{
    FILE* fi;
    unsigned char f;
    v2swf_internal_t* i = (v2swf_internal_t*)v2swf->internal;
    logf("v2swf_backpatch %s\n", filename);
    if(!i) {
	printf("call backpatch before close\n");fflush(stdout);
    }
    fi = fopen(filename, "rb+");
    if(!fi) {
	printf("can't open %s\n", filename);
	exit(1);
    }
    fseek(fi, 4, SEEK_SET);
    f = i->filesize      ;fwrite(&f,1,1,fi);
    f = i->filesize >> 8 ;fwrite(&f,1,1,fi);
    f = i->filesize >> 16;fwrite(&f,1,1,fi);
    f = i->filesize >> 24;fwrite(&f,1,1,fi);
    if(i->version<6) {
	/* no compression- we can backpatch the frames too */
	fseek(fi, i->headersize-2, SEEK_SET);
	f = i->frames        ;fwrite(&f,1,1,fi);
	f = i->frames >> 8   ;fwrite(&f,1,1,fi);
    }
    fclose(fi);
    if(i->fixheader) {
	SWF tmp;
	int fi;
	logf("v2swf_backpatch %s - fix header\n", filename);
	memset(&tmp, 0, sizeof(tmp));
	fi = open(filename, O_RDONLY|O_BINARY);
	if(fi>=0) {
	    if(swf_ReadSWF(fi, &tmp)>=0) {
		close(fi);
		fi = open(filename, O_WRONLY|O_BINARY|O_TRUNC|O_CREAT, 0666);
		if(fi>=0) {
		    swf_WriteSWC(fi, &tmp);
		    close(fi);
		    logf("v2swf_backpatch %s - fix header: success\n", filename);
		}
	    }
	}
    }
}

float v2swf_getprogress(v2swf_t*v2swf)
{
    float* p;
    v2swf_internal_t* i;
    logf("v2swf_getprogress()");
    if(!v2swf || !v2swf->internal) {
	return 0.0;
    }
    i = (v2swf_internal_t*)v2swf->internal;

    p = (float*)videoreader_getinfo(i->video, "position");

    if(p) {
	return *p;
    } else {
	float f = i->frames/1500.0; /*fake*/
	if(f>1.0)
	    return 1.0;
	else
	    return f;
    }
}

void v2swf_setvideoparameter(videoreader_t*v, char*name, char*value)
{
    logf("v2swf_setvideoparameter()");
    videoreader_setparameter(v, name, value);
}
