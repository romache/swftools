/* swfoutput.h
   Header file for swfoutput.cc (and swfoutput_x11.cc).

   Part of the swftools package.

   Copyright (c) 2001 Matthias Kramm <kramm@quiss.org> 

   This file is distributed under the GPL, see file COPYING for details */

#ifndef __swfoutput_h__
#define __swfoutput_h__

#include <t1lib.h>
extern "C" {
#include "../lib/rfxswf.h"
}

extern int enablezlib; //default:0
extern int opennewwindow; //default:0
extern int ignoredraworder; //default:0
extern int drawonlyshapes; //default:0
extern int jpegquality; //default:100;
extern int storeallcharacters; // default:0
extern int insertstoptag; //default:0

typedef long int twip;

struct swfmatrix {
    double m11,m12,m21,m22,m13,m23;
};

struct swfcoord {
    twip x;
    twip y;
};

class SWFFont
{
    T1_OUTLINE**outline;
    char**charname;
    int*width;
    char*used;

    char*name;
    int charnum;

    U16*char2swfcharid;
    U16*swfcharid2char;
    int swfcharpos;

    char**standardtable;
    int standardtablesize;

    public:
    
    int t1id;
    char*fontid;
    unsigned int swfid;

    SWFFont(char*name, int t1id, char*filename);
    SWFFont::~SWFFont();
    T1_OUTLINE*getOutline(char*charname);
    int getSWFCharID(char*name, int charnr);
    int getWidth(char*name);
    char*getName();
    char*getCharName(int t);
    int getCharWidth(int t) {return width[t];}
};

struct swfoutput 
{
    //int t1font;
    double fontm11,fontm12,fontm21,fontm22;
    unsigned short int linewidth;
    SWFFont*font;
    RGBA strokergb;
    RGBA fillrgb;
};

#define DRAWMODE_STROKE 1
#define DRAWMODE_FILL 2
#define DRAWMODE_EOFILL 3
#define DRAWMODE_CLIP 4
#define DRAWMODE_EOCLIP 5

void swfoutput_init(struct swfoutput*, char*filename, int sizex, int sizey);
void swfoutput_setprotected(); //write PROTECT tag

void swfoutput_newpage(struct swfoutput*);

void swfoutput_setfont(struct swfoutput*, char*fontid, int t1font, char*filename);
int swfoutput_queryfont(struct swfoutput*, char*fontid);
void swfoutput_setdrawmode(struct swfoutput*, int drawmode);
void swfoutput_setfillcolor(struct swfoutput*, unsigned char r, unsigned char g, unsigned char b, unsigned char a);
void swfoutput_setstrokecolor(struct swfoutput*, unsigned char r, unsigned char g, unsigned char b, unsigned char a);
void swfoutput_setfontmatrix(struct swfoutput*,double,double,double,double);
void swfoutput_setlinewidth(struct swfoutput*, double linewidth);

void swfoutput_drawchar(struct swfoutput*,double x,double y,char*a, int charnr);
void swfoutput_drawpath(struct swfoutput*, T1_OUTLINE*outline, struct swfmatrix*m);
void swfoutput_startclip(struct swfoutput*, T1_OUTLINE*outline, struct swfmatrix*m);
void swfoutput_endclip(struct swfoutput*);
int swfoutput_drawimagejpeg(struct swfoutput*, RGBA*pic, int sizex,int sizey, 
	double x1,double y1,
	double x2,double y2,
	double x3,double y3,
	double x4,double y4);
int swfoutput_drawimagelossless(struct swfoutput*, RGBA*pic, int sizex, int sizey,
	double x1,double y1,
	double x2,double y2,
	double x3,double y3,
	double x4,double y4);
int swfoutput_drawimagelosslessN(struct swfoutput*, U8*pic, RGBA*pal, int sizex, int sizey,
	double x1,double y1,
	double x2,double y2,
	double x3,double y3,
	double x4,double y4, int n);
void swfoutput_drawimageagain(struct swfoutput*, int id, int sizex, int sizey,
	double x1,double y1,
	double x2,double y2,
	double x3,double y3,
	double x4,double y4);

void swfoutput_linktopage(struct swfoutput*, int page, swfcoord*points);
void swfoutput_linktourl(struct swfoutput*, char*url, swfcoord*points);
void swfoutput_namedlink(struct swfoutput*obj, char*name, swfcoord*points);

void swfoutput_destroy(struct swfoutput*);

#endif //__swfoutput_h__
