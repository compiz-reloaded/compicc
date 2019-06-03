/**
 *  @file     compicc.c
 *
 *  @brief    a compiz desktop colour management plug-in
 *
 *  @author   Kai-Uwe Behrmann, based on Tomas' color filter, based on Gerhard
 *            Fürnkranz' GLSL ppm_viewer
 *  @par Copyright:
 *            2008 (C) Gerhard Fürnkranz, 2008 (C) Tomas Carnecky,
 *            2009-2016 (C) Kai-Uwe Behrmann
 *  @par License:
 *            new BSD <http://www.opensource.org/licenses/bsd-license.php>
 *  @since    2009/02/23
 */


/* strdup needs _BSD_SOURCE */
#define _BSD_SOURCE
#include <assert.h>
#include <math.h>     // floor()
#include <string.h>   // http://www.opengroup.org/onlinepubs/009695399/functions/strdup.html
#include <sys/time.h>
#include <time.h>
#include <unistd.h>   // getpid()

#include <stdarg.h>
#include <icc34.h>

#define GL_GLEXT_PROTOTYPES

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <X11/extensions/Xfixes.h>

#define HAVE_XRANDR
#ifdef HAVE_XRANDR
#include <X11/extensions/Xrandr.h>
#endif

#include <compiz-common.h>

#include <oyranos.h>
#include <oyranos_devices.h>
#include <oyranos_threads.h>
#include <oyConversion_s.h>
#include <oyFilterGraph_s.h>
#include <oyFilterNode_s.h>
#include <oyRectangle_s.h>

#include <X11/Xcm/Xcm.h>
#include <X11/Xcm/XcmEvents.h>


#define OY_COMPIZ_VERSION (COMPIZ_VERSION_MAJOR * 10000 + COMPIZ_VERSION_MINOR * 100 + COMPIZ_VERSION_MICRO)
#if OY_COMPIZ_VERSION < 708
#define oyCompLogMessage(disp_, plug_in_name, debug_level, format_, ... ) \
        compLogMessage( disp_, plug_in_name, debug_level, format_, __VA_ARGS__ )
#else
#define oyCompLogMessage( disp_, plug_in_name, debug_level, format_, ... ) \
{ if(oy_debug) \
    oyMessageFunc_p( oyMSG_DBG, NULL, format_, __VA_ARGS__); \
  else \
    compLogMessage( plug_in_name, debug_level, format_, __VA_ARGS__ ); \
}
#endif

#if OY_COMPIZ_VERSION < 900
#include <compiz-core.h>
#else
#define CompBool bool
#endif

#if OYRANOS_VERSION < 201
#define oyConversion_GetGraph( conversion ) oyFilterGraph_FromNode( conversion->input, 0)
#endif

/* Uncomment the following line if you want to enable debugging output */
#define PLUGIN_DEBUG 1

/**
 * The 3D lookup texture has 64 points in each dimension, using 16 bit integers.
 * That means each active region will use 1.5MiB of texture memory.
 */
#define GRIDPOINTS 64

static signed long colour_desktop_region_count = -1;
/**
 *  The stencil ID is a property of each window region to identify the used
 *  bit plane in the stencil buffer.
 *  Each screen context obtains a different range of IDs (i).
 *  j is the actual region in the window.
 */
#define STENCIL_ID ( 1 + colour_desktop_region_count * i + pw->stencil_id_start + j )

#define HAS_REGIONS(pw) (pw->nRegions > 1)

#define WINDOW_BORDER 30

#if defined(PLUGIN_DEBUG)
#define DBG  printf("%s:%d %s() %.02f\n", DBG_ARGS);
#else
#define DBG
#endif

static int icc_profile_flags = 0;

#define DBG_STRING " %s:%d %s() %.02f "
#define DBG_ARGS (strrchr(__FILE__,'/') ? strrchr(__FILE__,'/')+1 : __FILE__),__LINE__,__func__,(double)clock()/CLOCKS_PER_SEC
#if defined(PLUGIN_DEBUG)
#define START_CLOCK(text) fprintf( stderr, DBG_STRING text " - ", DBG_ARGS );
#define END_CLOCK         fprintf( stderr, "%.02f\n", (double)clock()/CLOCKS_PER_SEC );
#else
#define START_CLOCK(text)
#define END_CLOCK
#endif

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */
void* oyAllocateFunc_           (size_t        size);
void  oyDeAllocateFunc_         (void *        data);
#ifdef __cplusplus
}
#endif /* __cplusplus */


void* cicc_alloc                (size_t        size) { void * p = oyAllocateFunc_(size); memset(p,0,size); return p; }
void  cicc_free                 (void *        data) { oyDeAllocateFunc_(data); }

typedef CompBool (*dispatchObjectProc) (CompPlugin *plugin, CompObject *object, void *privateData);

/** Be active once and then not again. */
static int colour_desktop_can = 1;

/* last check time */
static time_t icc_color_desktop_last_time = 0;

/**
 *  All data to create and use a color conversion.
 *  Included are OpenGL texture, the source ICC profile for reference and the
 *  target profile for the used monitor.
 */
typedef struct {
  oyProfile_s * src_profile;         /* the data profile or device link */
  oyProfile_s * dst_profile;         /* the monitor profile or none */
  char * output_name;                /* the intented output device */
  GLushort clut[GRIDPOINTS][GRIDPOINTS][GRIDPOINTS][3]; /* lookup table */
  GLuint glTexture;                  /* texture reference */
  GLfloat scale, offset;             /* texture parameters */
  int ref;                           /* reference counter */
} PrivColorContext;

/**
 * The XserverRegion is dereferenced when the client sets it on a window.
 * This allows clients to change the region as the window is resized.
 * The profile is resolved as soon as the client uploads the regions into the
 * window. That means clients need to upload profiles first and then the 
 * regions. Otherwise the plugin won't be able to find the profile and no color
 * transformation will be done.
 */
typedef struct {
  /* These members are only valid when this region is part of the
   * active stack range. */
  uint8_t md5[16];
  PrivColorContext ** cc;
  Region xRegion;
} PrivColorRegion;

/**
 * Output profiles are currently only fetched using XRandR. For backwards 
 * compatibility the code should fall back to root window properties 
 * (XCM_ICC_V0_3_TARGET_PROFILE_IN_X_BASE).
 */
typedef struct {
  char name[32];
  PrivColorContext cc;
  XRectangle xRect;
} PrivColorOutput;


static CompMetadata pluginMetadata;

static int core_priv_index = -1;
static int display_priv_index = -1;
static int screen_priv_index = -1;
static int window_priv_index = -1;

typedef struct {
  int childPrivateIndex;

  ObjectAddProc objectAdd;
} PrivCore;

typedef struct {
  int childPrivateIndex;

  HandleEventProc handleEvent;

  /* Window properties */
  Atom iccColorProfiles;
  Atom iccColorRegions;
  Atom iccColorOutputs;
  Atom iccColorDesktop;
  Atom netDesktopGeometry;
  Atom iccDisplayAdvanced;
} PrivDisplay;

typedef struct {
  int childPrivateIndex;

  /* hooked functions */
  DrawWindowProc drawWindow;
  DrawWindowTextureProc drawWindowTexture;

  /* compiz fragement function */
  int function, param, unit;
  int function_2, param_2, unit_2;

  /* XRandR outputs and the associated profiles */
  unsigned long nContexts;
  PrivColorOutput *contexts;
} PrivScreen;

typedef struct {
  /* start of stencil IDs + nRegions need to be reserved for this window,
   * inside each monitors stencil ID range */
  unsigned long stencil_id_start;

  /* regions attached to the window */
  unsigned long nRegions;
  PrivColorRegion *pRegion;

  /* old absolute region */
  oyRectangle_s * absoluteWindowRectangleOld;

  /* active stack range */
  unsigned long active;

  /* active XRandR output */
  char *output;
} PrivWindow;

static Region absoluteRegion(CompWindow *w, Region region);
static void damageWindow(CompWindow *w, void *closure);
static void addWindowRegionCount(CompWindow *w, void * count);
oyPointer  pluginGetPrivatePointer   ( CompObject        * o );
static void updateOutputConfiguration( CompScreen        * s,
                                       CompBool            init,
                                       int                 screen );
static int updateIccColorDesktopAtom ( CompScreen        * s,
                                       PrivScreen        * ps,
                                       int                 request );
static oyPointer   getScreenProfile  ( CompScreen        * s,
                                       int                 screen,
                                       int                 server,
                                       size_t            * size );
int            needUpdate            ( Display           * display );
static void    moveICCprofileAtoms   ( CompScreen        * s,
                                       int                 screen,
                                       int                 init );
void           cleanDisplayProfiles  ( CompScreen        * s );
static int     getDisplayAdvanced    ( CompScreen        * s,
                                       int                 screen );
static int     getDeviceProfile      ( CompScreen        * s,
                                       PrivScreen        * ps,
                                       oyConfig_s        * device,
                                       int                 screen );
oyProfile_s *  profileFromMD5        ( uint8_t           * md5 );
static void    setupOutputTable      ( CompScreen        * s,
                                       oyConfig_s        * device,
                                       int                 screen );
static void    setupColourTable      ( PrivColorContext  * ccontext,
                                       int                 advanced,
                                       CompScreen        * s );
static void changeProperty           ( Display           * display,
                                       Atom                target_atom,
                                       int                 type,
                                       void              * data,
                                       unsigned long       size );
static void *fetchProperty(Display *dpy, Window w, Atom prop, Atom type, unsigned long *n, Bool del);
static oyStructList_s * pluginGetPrivatesCache ();

static void *compObjectGetPrivate(CompObject *o)
{
  oyPointer private_data = pluginGetPrivatePointer( o );
  return private_data;
}

static void compObjectFreePrivate(CompObject *o)
{
  int index = -1;
  switch(o->type)
  {
    case COMP_OBJECT_TYPE_CORE:
           index = core_priv_index;
         break;
    case COMP_OBJECT_TYPE_DISPLAY:
           index = display_priv_index;
         break;
    case COMP_OBJECT_TYPE_SCREEN:
           index = screen_priv_index;
         break;
    case COMP_OBJECT_TYPE_WINDOW:
           index = window_priv_index;
         break;
  }

  if(index < 0)
    return;

  oyPointer ptr = o->privates[index].ptr;
  o->privates[index].ptr = NULL;
  if(ptr)
    cicc_free(ptr);
}


/**
 * Xcolor helper functions. I didn't really want to put them into the Xcolor
 * library.
 * Other window managers are free to copy those when needed.
 */
static inline XcolorProfile *XcolorProfileNext(XcolorProfile *profile)
{
	unsigned char *ptr = (unsigned char *) profile;
	return (XcolorProfile *) (ptr + sizeof(XcolorProfile) + ntohl(profile->length));
}

static inline unsigned long XcolorProfileCount(void *data, unsigned long nBytes)
{
	unsigned long count = 0;
  XcolorProfile * ptr;

  for (ptr = (XcolorProfile*)data;
       (uintptr_t)ptr < (uintptr_t)data + nBytes;
       ptr = XcolorProfileNext(ptr))
    ++count;

	return count;
}

static inline XcolorRegion *XcolorRegionNext(XcolorRegion *region)
{
  unsigned char *ptr = (unsigned char *) region;
  return (XcolorRegion *) (ptr + sizeof(XcolorRegion));
}

static inline unsigned long XcolorRegionCount(void *data OY_UNUSED, unsigned long nBytes)
{
  return nBytes / sizeof(XcolorRegion);
}

/**
 * Helper function to convert a MD5 into a readable string.
 */
static const char *md5string(const uint8_t md5[16])
{
	static char buffer[33] = {0};
	const uint32_t * h = (const uint32_t*)md5;

	sprintf( buffer, "%x%x%x%x", h[0],h[1],h[2],h[3]);

	return buffer;
}

/**
 * Here begins the real code
 */

static int getFetchTarget(CompTexture *texture)
{
  if (texture->target == GL_TEXTURE_2D) {
    return COMP_FETCH_TARGET_2D;
  } else {
    return COMP_FETCH_TARGET_RECT;
  }
}

/**
 * The shader is the same for all windows and profiles. It only depends on the
 * 3D texture and two environment variables.
 */
static int getProfileShader(CompScreen *s, CompTexture *texture, int param, int unit)
{
  PrivScreen *ps = compObjectGetPrivate((CompObject *) s);
  int function = -1;

  if (ps->function && ps->param == param && ps->unit == unit)
    return ps->function;

  if(ps->function_2 && ps->param_2 == param && ps->unit_2 == unit)
    return ps->function_2;

  if( ps->function_2)
    destroyFragmentFunction(s, ps->function_2);
  /*else if(ps->function)  should not happen as funcition is statical cached
    destroyFragmentFunction(s, ps->function); */

  /* shaders are programmed using ARB GPU assembly language */
  CompFunctionData *data = createFunctionData();

  addTempHeaderOpToFunctionData(data, "temp");

  addFetchOpToFunctionData(data, "output", NULL, getFetchTarget(texture));

  /* store alpha */
  addDataOpToFunctionData(data, "MOV temp, output;");

  /* needed, but why? */
  addDataOpToFunctionData(data, "MAD output, output, program.env[%d], program.env[%d];", param, param + 1);

  /* colour transform through a texture lookup */
  addDataOpToFunctionData(data, "TEX output, output, texture[%d], 3D;", unit);

  /* multiply alpha */
  addDataOpToFunctionData(data, "MUL output, temp.a, output;");

  addColorOpToFunctionData (data, "output", "output");

  function = createFragmentFunction(s, "compicc", data);



  if(ps->param == -1)
  {
    ps->function = function;
    ps->param = param;
    ps->unit = unit;
    return ps->function;
  } else
  {
    ps->function_2 = function;
    ps->param_2 = param;
    ps->unit_2 = unit;
    return ps->function_2;
  }
}

/**
 * Converts a server-side region to a client-side region.
 */
static Region convertRegion(Display *dpy, XserverRegion src)
{
  Region ret = XCreateRegion();

  int nRects = 0;
  XRectangle *rect = XFixesFetchRegion(dpy, src, &nRects);

  for (int i = 0; i < nRects; ++i) {
    XUnionRectWithRegion(&rect[i], ret, ret);
  }

  XFree(rect);

  return ret;
}

static Region windowRegion( CompWindow * w )
{
  Region r = XCreateRegion();
  XRectangle rect = {0,0,w->serverWidth, w->serverHeight};
  XUnionRectWithRegion( &rect, r, r );
 return r;
}

/**
 * Generic function to fetch a window property.
 */
static void *fetchProperty(Display *dpy, Window w, Atom prop, Atom type, unsigned long *n, Bool del)
{
  Atom actual;
  int format;
  unsigned long left;
  unsigned char *data;
  const char * atom_name = XGetAtomName( dpy, prop );

  XFlush( dpy );

  int result = XGetWindowProperty( dpy, w, prop, 0, ~0, del, type, &actual,
                                   &format, n, &left, &data );

  oyCompLogMessage(d, "compicc", CompLogLevelDebug, DBG_STRING "XGetWindowProperty w: %lu atom: %s n: %lu left: %lu", DBG_ARGS, w, atom_name, *n, left  );

  if(del)
  printf( "compicc erasing atom %lu\n", prop );
  if (result == Success)
    return (void *) data;

  return NULL;
}

/**
 * Called when new profiles have been attached to the root window. Fetches
 * these and saves them in a local database.
 */ 
static void updateScreenProfiles(CompScreen *s)
{
  CompDisplay *d = s->display;
  PrivDisplay *pd = compObjectGetPrivate((CompObject *) d);

  /* Fetch the profiles */
  unsigned long nBytes;
  int screen = DefaultScreen( s->display->display );
  void *data = fetchProperty(d->display,
                                   XRootWindow( s->display->display, screen ),
                                   pd->iccColorProfiles,
                                   XA_CARDINAL, &nBytes, True);
  if (data == NULL)
    return;

  uint32_t exact_hash_size = 0;
  oyHash_s * entry;
  oyProfile_s * prof = NULL;
  oyStructList_s * cache = pluginGetPrivatesCache();
  int n = 0;

  /* Grow or shring the array as needed. */
  unsigned long count = XcolorProfileCount(data, nBytes);

  /* Copy the profiles into the array, and create the Oyranos handles. */
  XcolorProfile *profile = data;
  for (unsigned long i = 0; i < count; ++i)
  {
    const char * hash_text = md5string(profile->md5);
    entry = oyStructList_GetHash( cache, exact_hash_size, hash_text );
    prof = (oyProfile_s *) oyHash_GetPointer( entry, oyOBJECT_PROFILE_S);
    /* XcolorProfile::length == 0 means the clients wants to delete the profile. */
    if( ntohl(profile->length) )
    {
      if(!prof)
      {
        prof = oyProfile_FromMem( htonl(profile->length), profile + 1, 0,NULL );

        if(!prof)
        {
          /* If creating the Oyranos profile fails, don't try to parse any further profiles and just quit. */
          oyCompLogMessage(d, "compicc", CompLogLevelWarn, "Couldn't create Oyranos profile %s", hash_text );
          goto out;
        }

        oyHash_SetPointer( entry, (oyStruct_s*) prof );
        ++n;
      }
    }

    profile = XcolorProfileNext(profile);
  }

#if defined(PLUGIN_DEBUG_)
  oyCompLogMessage(d, "compicc", CompLogLevelDebug, "Added %d of %d screen profiles",
		      n, count);
#endif

  out:
  XFree(data);
}

oyProfile_s *  profileFromMD5        ( uint8_t           * md5 )
{
  uint32_t exact_hash_size = 0;
  oyProfile_s * prof = NULL;
  oyStructList_s * cache = pluginGetPrivatesCache();

  /* Copy the profiles into the array, and create the Oyranos handles. */
  const char * hash_text = md5string(md5);
  prof = (oyProfile_s *) oyStructList_GetHashStruct( cache, exact_hash_size,
                                               hash_text, oyOBJECT_PROFILE_S );

  return prof;
}

/**
 * Called when new regions have been attached to a window. Fetches these and
 * saves them in the local list.
 */
static void updateWindowRegions(CompWindow *w)
{
  PrivWindow *pw = compObjectGetPrivate((CompObject *) w);

  CompDisplay *d = w->screen->display;
  PrivDisplay *pd = compObjectGetPrivate((CompObject *) d);
  PrivScreen *ps = compObjectGetPrivate((CompObject *) w->screen);

  /* free existing data structures */
  for (unsigned long i = 0; i < pw->nRegions; ++i)
  {
    if (pw->pRegion[i].xRegion != 0) {
      XDestroyRegion(pw->pRegion[i].xRegion);
    }
    if(pw->pRegion[i].cc)
    {
      for(unsigned long j = 0; j < ps->nContexts; ++j)
      {
        if(pw->pRegion[i].cc[j])
        {
          oyProfile_Release( &pw->pRegion[i].cc[j]->dst_profile );
          oyProfile_Release( &pw->pRegion[i].cc[j]->src_profile );
          if(&pw->pRegion[i].cc[j]->glTexture)
            glDeleteTextures( 1, &pw->pRegion[i].cc[j]->glTexture );
          cicc_free( pw->pRegion[i].cc[j] );
          pw->pRegion[i].cc[j] = NULL;
        }
        else
          break;
      }
      cicc_free( pw->pRegion[i].cc ); pw->pRegion[i].cc = 0;
    }
  }
  if (pw->nRegions)
    cicc_free(pw->pRegion);
  pw->nRegions = 0;
  oyRectangle_Release( &pw->absoluteWindowRectangleOld );


  /* fetch the regions */
  unsigned long nBytes;
  void *data = fetchProperty( d->display, w->id, pd->iccColorRegions,
                              XA_CARDINAL, &nBytes, False );

  /* allocate the list */
  unsigned long count = 1;
  if(data)
    count += XcolorRegionCount(data, nBytes + 1);

  if(oy_debug)
  fprintf( stderr, DBG_STRING"XcolorRegionCount+1=%lu\n", DBG_ARGS,
           count );

  pw->pRegion = (PrivColorRegion*) cicc_alloc(count * sizeof(PrivColorRegion));
  if (pw->pRegion == NULL)
    goto out;

  /* get the complete windows region and put it at the end */
  pw->pRegion[count-1].xRegion = windowRegion( w );


  /* fill in the possible application region(s) */
  XcolorRegion *region = data;
  Region wRegion = pw->pRegion[count-1].xRegion;
  for (unsigned long i = 0; i < (count - 1); ++i)
  {
    uint8_t n[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    pw->pRegion[i].xRegion = convertRegion( d->display, ntohl(region->region) );
    memcpy( pw->pRegion[i].md5, region->md5, 16 );

    /* substract a application region from the window region */
    XSubtractRegion( wRegion, pw->pRegion[i].xRegion, wRegion );

    if(memcmp(region->md5,n,16) != 0)
    {
      pw->pRegion[i].cc = (PrivColorContext**)cicc_alloc( (ps->nContexts + 1) *
                                                    sizeof(PrivColorContext*));
      if(!pw->pRegion[i].cc)
      {
        printf( DBG_STRING "Could not allocate contexts. Stop!\n",
                DBG_ARGS );
        goto out;
      }

      for(unsigned long j = 0; j < ps->nContexts; ++j)
      {
        pw->pRegion[i].cc[j] = (PrivColorContext*) cicc_alloc(
                                                     sizeof(PrivColorContext) );

        if(!pw->pRegion[i].cc[j])
        {
          printf( DBG_STRING "Could not allocate context. Stop!\n",
                  DBG_ARGS );
          goto out;
        }

        if(ps && ps->nContexts > 0)
        {
          pw->pRegion[i].cc[j]->dst_profile = oyProfile_Copy(
                                            ps->contexts[j].cc.dst_profile, 0 );

          if(!pw->pRegion[i].cc[j]->dst_profile)
          {
            printf( DBG_STRING "output 0 not ready\n",
                    DBG_ARGS );
            continue;
          }
          pw->pRegion[i].cc[j]->src_profile = profileFromMD5(region->md5);
          fprintf( stderr, DBG_STRING"region->md5: %s\n", DBG_ARGS,
                   oyProfile_GetText( pw->pRegion[i].cc[j]->src_profile, oyNAME_DESCRIPTION ) );

          pw->pRegion[i].cc[j]->output_name = strdup(
                                               ps->contexts[j].cc.output_name );
        } else
          printf( DBG_STRING "output_name: %s\n",
                  DBG_ARGS, ps->contexts[j].cc.output_name);

        if(pw->pRegion[i].cc[j]->src_profile)
          setupColourTable( pw->pRegion[i].cc[j],
                            getDisplayAdvanced(w->screen, 0), w->screen );
        else
          printf( DBG_STRING "region %lu on %lu has no source profile!\n",
                  DBG_ARGS, i, j );
      }
    } else if(oy_debug)
      fprintf( stderr, DBG_STRING"no region->md5 %lu cc=0x%lx %d,%d,%dx%d\n", DBG_ARGS,
               i, (unsigned long)pw->pRegion[i].cc, pw->pRegion[i].xRegion->extents.x1,
               pw->pRegion[i].xRegion->extents.y1,
               pw->pRegion[i].xRegion->extents.x2-pw->pRegion[i].xRegion->extents.x1,
               pw->pRegion[i].xRegion->extents.y2-pw->pRegion[i].xRegion->extents.y1
 );

    region = XcolorRegionNext(region);
  }

  pw->nRegions = count;
  pw->active = 1;

  pw->absoluteWindowRectangleOld = oyRectangle_NewWith( 0, 0, w->serverWidth,
                                                        w->serverHeight, 0 );

  addWindowDamage(w);

out:
  XFree(data);
#if defined(PLUGIN_DEBUG_)
  if(count > 1)
  oyCompLogMessage(d, "compicc", CompLogLevelDebug, "Added %d regions",
		      count);
#endif

}


/**
 * Called when the window target (_ICC_COLOR_TARGET) has been changed.
 */
static void updateWindowOutput(CompWindow *w)
{
  PrivWindow *pw = compObjectGetPrivate((CompObject *) w);

  CompDisplay *d = w->screen->display;
  PrivDisplay *pd = compObjectGetPrivate((CompObject *) d);

  if (pw->output)
    XFree(pw->output);

  unsigned long nBytes;
  pw->output = fetchProperty(d->display, w->id, pd->iccColorOutputs, XA_STRING, &nBytes, False);

  if(!pw->nRegions)
    addWindowDamage(w);
}

static void cdCreateTexture( PrivColorContext *ccontext )
{
    glBindTexture(GL_TEXTURE_3D, ccontext->glTexture);

    ccontext->scale = (GLfloat) (GRIDPOINTS - 1) / GRIDPOINTS;
    ccontext->offset = (GLfloat) 1.0 / (2 * GRIDPOINTS);


    glGenTextures(1, &ccontext->glTexture);
    glBindTexture(GL_TEXTURE_3D, ccontext->glTexture);

    fprintf( stderr, DBG_STRING"glTexture=%d\n", DBG_ARGS,
             ccontext->glTexture );

    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glTexImage3D( GL_TEXTURE_3D, 0, GL_RGB16, GRIDPOINTS,GRIDPOINTS,GRIDPOINTS,
                  0, GL_RGB, GL_UNSIGNED_SHORT, ccontext->clut);
}

/* returned data is owned by user;
 * free with XFree(returned_data)
 */
static oyPointer   getScreenProfile  ( CompScreen        * s,
                                       int                 screen,
                                       int                 server,
                                       size_t            * size )
{
  char num[12];
  Window root = RootWindow( s->display->display, 0 );
  char * icc_profile_atom = (char*)cicc_alloc( 1024 );
  Atom a;
  oyPointer data;
  unsigned long n = 0;

  if(!icc_profile_atom) return 0;

  snprintf( num, 12, "%d", (int)screen );
  if(server)
  snprintf( icc_profile_atom, 1024, XCM_DEVICE_PROFILE"%s%s", 
            screen ? "_" : "", screen ? num : "" );
  else
  snprintf( icc_profile_atom, 1024, XCM_ICC_V0_3_TARGET_PROFILE_IN_X_BASE"%s%s", 
            screen ? "_" : "", screen ? num : "" );


  a = XInternAtom(s->display->display, icc_profile_atom, False);

  oyCompLogMessage( s->display, "compicc", CompLogLevelDebug,
                    DBG_STRING"fetching profile from %s atom: %d",
                    DBG_ARGS,
                    icc_profile_atom, a);
  
  data = fetchProperty( s->display->display, root, a, XA_CARDINAL,
                        &n, False);
  oyCompLogMessage( s->display, "compicc", CompLogLevelDebug,
                    DBG_STRING"fetching %s, found %lu: %s",
                    DBG_ARGS,
                    icc_profile_atom,
                    n, (data == NULL ? "no data":"some data obtained") );
  *size = (size_t)n;
  cicc_free(icc_profile_atom);
  return data;
}

static void changeProperty           ( Display           * display,
                                       Atom                target_atom,
                                       int                 type,
                                       void              * data,
                                       unsigned long       size )
{
  const char * atom_name = XGetAtomName( display, target_atom );
  oyCompLogMessage( display, "compicc", CompLogLevelDebug,
                    DBG_STRING"XChangeProperty atom: %s size: %lu",
                    DBG_ARGS,
                    atom_name, size );
    XChangeProperty( display, RootWindow( display, 0 ),
                     target_atom, type, 8, PropModeReplace,
                     data, size );
}

static void    moveICCprofileAtoms   ( CompScreen        * s,
                                       int                 screen,
                                       int                 init )
{
  {
    oyOptions_s * opts = 0,
                * result = 0;

    const char * display_name = strdup(XDisplayString(s->display->display));

    oyOptions_SetFromString( &opts, "////display_name",
                           display_name, OY_CREATE_NEW );

    oyOptions_SetFromInt( &opts, "////screen", screen, 0, OY_CREATE_NEW );
    oyOptions_SetFromInt( &opts, "////setup", init, 0, OY_CREATE_NEW );
    oyCompLogMessage( s->display, "compicc", CompLogLevelDebug,
                  DBG_STRING "Moving profiles on %s: for screen %d setup %d",
                  DBG_ARGS, display_name, screen, init );
    //fprintf( stderr, "Moving profiles on %s: for screen %d setup %d\n",
    //                 display_name, screen, init );
    oyOptions_Handle( "//" OY_TYPE_STD "/move_color_server_profiles",
                                opts,"move_color_server_profiles",
                                &result );
    oyOptions_Release( &opts );
    oyOptions_Release( &result );
    return;
  }
}

static int     getDeviceProfile      ( CompScreen        * s,
                                       PrivScreen        * ps,
                                       oyConfig_s        * device,
                                       int                 screen )
{
  PrivColorOutput * output = &ps->contexts[screen];
  oyOption_s * o = 0;
  oyRectangle_s * r = 0;
  const char * device_name = 0;
  char num[12];
  int error = 0, t_err = 0;

  snprintf( num, 12, "%d", (int)screen );

    o = oyConfig_Find( device, "device_rectangle" );
    if( !o )
    {
      oyCompLogMessage( s->display, "compicc", CompLogLevelWarn,
                      DBG_STRING"monitor rectangle request failed", DBG_ARGS);
      return 1;
    }
    r = (oyRectangle_s*) oyOption_GetStruct( o, oyOBJECT_RECTANGLE_S );
    if( !r )
    {
      oyCompLogMessage( s->display, "compicc", CompLogLevelWarn,
                      DBG_STRING"monitor rectangle request failed", DBG_ARGS);
      return 1;
    }
    oyOption_Release( &o );

    output->xRect.x = oyRectangle_GetGeo1( r, 0 );
    output->xRect.y = oyRectangle_GetGeo1( r, 1 );
    output->xRect.width = oyRectangle_GetGeo1( r, 2 );
    output->xRect.height = oyRectangle_GetGeo1( r, 3 );

    device_name = oyConfig_FindString( device, "device_name", 0 );
    if(device_name && device_name[0])
    {
      strcpy( output->name, device_name );

    } else
    {
       oyCompLogMessage( s->display, "compicc", CompLogLevelWarn,
       DBG_STRING "oyDevicesGet list answere included no device_name",DBG_ARGS);

       strcpy( output->name, num );
    }

    oyProfile_Release( &output->cc.dst_profile );

    size_t size = 0;
    int server = 1;
    /* try to get the device profile atom */
    oyPointer pp = getScreenProfile( s, screen, server, &size );

    /* check if the normal profile atom is a device profile */
    if(!pp)
    {
      server = 0;
      pp = getScreenProfile( s, screen, server, &size );
      if(!pp)
      oyCompLogMessage( s->display, "compicc", CompLogLevelDebug,
                      DBG_STRING "no server profile on %s, size: %d",
                      DBG_ARGS, output->name, (int)size);

      /* filter out ordinary sRGB */
      if(pp)
      {
        oyProfile_s * web = oyProfile_FromStd( oyASSUMED_WEB,
                                               icc_profile_flags, 0 );
        output->cc.dst_profile = oyProfile_FromMem( size, pp, 0,0 );
        if(oyProfile_Equal( web, output->cc.dst_profile ))
          oyProfile_Release( &output->cc.dst_profile );
        oyProfile_Release( &web );
      } else
      oyCompLogMessage( s->display, "compicc", CompLogLevelDebug,
                      DBG_STRING "no normal profile on %s, size: %d",
                      DBG_ARGS, output->name, (int)size);
    }
    if(pp)
    { XFree(pp); pp = NULL; }

    if(output->cc.dst_profile)
    {
      oyCompLogMessage( s->display, "compicc", CompLogLevelDebug,
                      DBG_STRING "reusing existing profile on %s, size: %d",
                      DBG_ARGS, output->name, (int)size);
    } else
    {
      oyOptions_s * options = 0;
      oyOptions_SetFromString( &options,
                   "//" OY_TYPE_STD "/config/command",
                                       "list", OY_CREATE_NEW );
      oyOptions_SetFromInt( &options,
                            "////icc_profile_flags",
                            icc_profile_flags, 0, OY_CREATE_NEW );
      oyOptions_SetFromString( &options,
                   "//" OY_TYPE_STD "/config/icc_profile.x_color_region_target",
                                       "yes", OY_CREATE_NEW );
      t_err = oyDeviceAskProfile2( device, options, &output->cc.dst_profile );
      if(t_err)
        oyCompLogMessage( s->display, "compicc", CompLogLevelDebug,
                      DBG_STRING "oyDeviceAskProfile2() returned an issue %s: %d",
                      DBG_ARGS, output->name, t_err);
      if(!output->cc.dst_profile || t_err == -1)
      {
        int old_t_err = t_err;
        t_err = oyDeviceGetProfile( device, options, &output->cc.dst_profile );
        oyCompLogMessage( s->display, "compicc", CompLogLevelDebug,
                      DBG_STRING "oyDeviceAskProfile2() has \"%s\" profile on %s: %d oyDeviceGetProfile() got -> \"%s\" %d",
                      DBG_ARGS, output->cc.dst_profile ? oyProfile_GetText(output->cc.dst_profile, oyNAME_DESCRIPTION):"----",
                      output->name, old_t_err, oyProfile_GetText(output->cc.dst_profile, oyNAME_DESCRIPTION), t_err);
      }
      oyOptions_Release( &options );
    }

    if(output->cc.dst_profile)
    {
      /* check that no sRGB is delivered */
      if(t_err)
      {
        oyProfile_s * web = oyProfile_FromStd( oyASSUMED_WEB, icc_profile_flags, 0 );
        if(oyProfile_Equal( web, output->cc.dst_profile ))
        {
          oyCompLogMessage( s->display, "compicc", CompLogLevelDebug,
                      DBG_STRING "Output %s ignoring sRGB fallback %d %d",
                      DBG_ARGS, output->name, error, t_err);
          oyProfile_Release( &output->cc.dst_profile );
          error = 1;
        }
        oyProfile_Release( &web );
      }
    } else
    {
      oyCompLogMessage( s->display, "compicc", CompLogLevelWarn,
                      DBG_STRING "Output %s: no ICC profile found %d",
                      DBG_ARGS, output->name, error);
      error = 1;
    }

  return error;
}

typedef struct pcc_s {
  PrivColorContext * ccontext;
  int advanced;
  CompScreen * screen;
} pcc_t;

static void * setupColourTable_cb( void * data )
{
  pcc_t * d = (pcc_t*)data;

  setupColourTable( d->ccontext, d->advanced, d->screen );
  updateOutputConfiguration( d->screen, FALSE, -1 );

  return NULL;
}
static void iccProgressCallback (    double              progress_zero_till_one,
                                     char              * status_text OY_UNUSED,
                                     int                 thread_id_,
                                     int                 job_id,
                                     oyStruct_s        * cb_progress_context )
{
  oyPointer_s * context = (oyPointer_s *) cb_progress_context;
  pcc_t * pcontext = (pcc_t*) oyPointer_GetPointer( context );
  printf( "%s() job_id: %d thread: %d %g\n", __func__, job_id, thread_id_,
          progress_zero_till_one );
  if(progress_zero_till_one >= 1.0)
  {
    setupColourTable_cb( pcontext );
    free(pcontext);
  }
}


static void    setupColourTable      ( PrivColorContext  * ccontext,
                                       int                 advanced,
                                       CompScreen        * s )
{
  oyConversion_s * cc;
  int error = 0;
  oyProfile_s * dst_profile = ccontext->dst_profile, * web = 0;

    if(!ccontext->dst_profile)
      dst_profile = web = oyProfile_FromStd( oyASSUMED_WEB, icc_profile_flags, 0 );

    {
      int flags = 0;
      int ** ptr;

      oyProfile_s * src_profile = ccontext->src_profile;
      oyOptions_s * options = 0;

      oyPixel_t pixel_layout = OY_TYPE_123_16;
      oyCompLogMessage(NULL, "compicc", CompLogLevelDebug,
             DBG_STRING "%s -> %s",
             DBG_ARGS, oyProfile_GetText( src_profile, oyNAME_DESCRIPTION ),
                       oyProfile_GetText( dst_profile, oyNAME_DESCRIPTION ) );

      /* skip web to web conversion */
      if(oyProfile_Equal( src_profile, web ))
      {
        oyCompLogMessage(NULL, "compicc", CompLogLevelDebug,
             DBG_STRING "src_profile == web",
             DBG_ARGS );
        goto clean_setupColourTable;
      }

      if(!src_profile)
        src_profile = oyProfile_FromStd( oyASSUMED_WEB, icc_profile_flags, 0 );

      if(!src_profile)
        oyCompLogMessage(NULL, "compicc", CompLogLevelWarn,
             DBG_STRING "Output %s: no oyASSUMED_WEB src_profile",
             DBG_ARGS, ccontext->output_name );

      /* optionally set advanced options from Oyranos */
      if(advanced)
        flags = oyOPTIONATTRIBUTE_ADVANCED;

      oyCompLogMessage( NULL, "compicc", CompLogLevelDebug,
                      DBG_STRING "oyConversion_Correct(///icc_color,%d,0) %s %s",
                      DBG_ARGS, flags, ccontext->output_name,
                      advanced?"advanced":"");
      oyImage_s * image_in = oyImage_Create( GRIDPOINTS,GRIDPOINTS*GRIDPOINTS,
                                             ccontext->clut,
                                             pixel_layout, src_profile, 0 );
      oyImage_s * image_out= oyImage_Create( GRIDPOINTS,GRIDPOINTS*GRIDPOINTS,
                                             ccontext->clut,
                                             pixel_layout, dst_profile, 0 );

      oyProfile_Release( &src_profile );

      oyJob_s * job = oyJob_New(0);
      job->cb_progress = iccProgressCallback;
      oyPointer_s * oy_ptr = oyPointer_New(0);
      pcc_t * pcc   = calloc( sizeof(pcc_t), 1 );
      pcc->ccontext = ccontext;
      pcc->advanced = advanced;
      pcc->screen = s;
      oyPointer_Set( oy_ptr,
                     __FILE__,
                     "struct pcc_s*",
                     pcc, 0, 0 );
      job->cb_progress_context = (oyStruct_s*) oyPointer_Copy( oy_ptr, 0 );
      oyOptions_MoveInStruct( &options, OY_BEHAVIOUR_STD "/expensive_callback", (oyStruct_s**)&job, OY_CREATE_NEW );
      /* wait no longer than approximately 1 seconds */
      oyOptions_SetFromString( &options, OY_BEHAVIOUR_STD "/expensive", "10", OY_CREATE_NEW );
      cc = oyConversion_CreateBasicPixels( image_in, image_out, options, 0 );
      if (cc == NULL)
      {
        oyCompLogMessage( NULL, "compicc", CompLogLevelWarn,
                      DBG_STRING "no conversion created for %s",
                      DBG_ARGS, ccontext->output_name);
        goto clean_setupColourTable;
      }
      oyOptions_Release( &options );

      error = oyOptions_SetFromString( &options,
                                     "//" OY_TYPE_STD "/config/display_mode", "1",
                                     OY_CREATE_NEW );
      error = oyConversion_Correct(cc, "//" OY_TYPE_STD "/icc_color", flags, options);
      if(error)
      {
        oyCompLogMessage( NULL, "compicc", CompLogLevelWarn,
                      DBG_STRING "oyConversion_Correct(///icc_color,%d,0) failed %s",
                      DBG_ARGS, flags, ccontext->output_name);
        goto clean_setupColourTable;
      }
      oyOptions_Release( &options );


      oyFilterGraph_s * cc_graph = oyConversion_GetGraph( cc );
      oyFilterNode_s * icc = oyFilterGraph_GetNode( cc_graph, -1, "///icc_color", 0 );

      uint32_t exact_hash_size = 0;
      char * hash_text = 0;
      const char * t = 0;
      {
        t = oyFilterNode_GetText( icc, oyNAME_NAME );
        if(t)
          hash_text = strdup(t);
      }
      oyHash_s * entry;
      oyArray2d_s * clut = NULL;
      oyStructList_s * cache = pluginGetPrivatesCache();
      entry = oyStructList_GetHash( cache, exact_hash_size, hash_text );
      clut = (oyArray2d_s*) oyHash_GetPointer( entry, oyOBJECT_ARRAY2D_S);
      oyFilterNode_Release( &icc );
      oyFilterGraph_Release( &cc_graph );

      oyCompLogMessage( NULL, "compicc", CompLogLevelDebug,
                      DBG_STRING "clut from cache %s %s",
                      DBG_ARGS, clut?"obtained":"no", hash_text );
      if(clut)
      {
        ptr = (int**)oyArray2d_GetData(clut);
        memcpy( ccontext->clut, ptr[0], 
                sizeof(GLushort) * GRIDPOINTS*GRIDPOINTS*GRIDPOINTS * 3 );
      } else
      {
        oyBlob_s * blob = oyFilterNode_ToBlob( icc, NULL );

        if(!blob)
        {
          oyConversion_Release( &cc );
          oyFilterNode_Release( &icc );

          oyOptions_SetFromString( &options, OY_DEFAULT_CMM_CONTEXT, "lcm2",
                               OY_CREATE_NEW );
          cc = oyConversion_CreateBasicPixels( image_in, image_out, options, 0 );
          if (cc == NULL)
          {
            oyCompLogMessage( NULL, "compicc", CompLogLevelWarn,
                      DBG_STRING "no conversion created for %s",
                      DBG_ARGS, ccontext->output_name);
            goto clean_setupColourTable;
          }
          oyOptions_Release( &options );
          error = oyOptions_SetFromString( &options,
                                     "//"OY_TYPE_STD"/config/display_mode", "1",
                                     OY_CREATE_NEW );
          error = oyConversion_Correct(cc, "//" OY_TYPE_STD "/icc_color", flags, options);
          icc = oyFilterGraph_GetNode( cc_graph, -1, "///icc_color", 0 );
          blob = oyFilterNode_ToBlob( icc, NULL );
          oyCompLogMessage( NULL, "compicc", CompLogLevelDebug,
                      DBG_STRING "created %s",
                      DBG_ARGS, t );
        }

        if(oy_debug)
        {
          oyOptions_s * node_opts = oyFilterNode_GetOptions( icc, 0 );
          oyProfile_s * dl;
          dl = oyProfile_FromMem( oyBlob_GetSize( blob ),
                                oyBlob_GetPointer( blob ), 0,0 );
          const char * fn;
          int j = 0;
          while((fn = oyProfile_GetFileName( dl, j )) != NULL)
            fprintf( stdout, " -> \"%s\"[%d]", fn?fn:"----", j++ );
          fprintf( stdout, "\n" );
          fprintf( stdout, "%s\n", oyOptions_GetText( node_opts, oyNAME_NAME ) );
        }

        uint16_t in[3];
        for (int r = 0; r < GRIDPOINTS; ++r)
        {
          in[0] = floor((double) r / (GRIDPOINTS - 1) * 65535.0 + 0.5);
          for (int g = 0; g < GRIDPOINTS; ++g) {
            in[1] = floor((double) g / (GRIDPOINTS - 1) * 65535.0 + 0.5);
            for (int b = 0; b < GRIDPOINTS; ++b)
            {
              in[2] = floor((double) b / (GRIDPOINTS - 1) * 65535.0 + 0.5);
              for(int j = 0; j < 3; ++j)
                /* BGR */
                ccontext->clut[b][g][r][j] = in[j];
            }
          }
        }

        clut = oyArray2d_Create( NULL, GRIDPOINTS*3, GRIDPOINTS*GRIDPOINTS,
                                 oyUINT16, NULL );

        error = oyConversion_RunPixels( cc, 0 );

        if(error)
        {
          oyCompLogMessage( NULL, "compicc", CompLogLevelWarn,
                      DBG_STRING "oyConversion_RunPixels() error: %d %s",
                      DBG_ARGS, error, ccontext->output_name);
          goto clean_setupColourTable;
        }

        ptr = (int**)oyArray2d_GetData(clut);
        memcpy( ptr[0], ccontext->clut,
                sizeof(GLushort) * GRIDPOINTS*GRIDPOINTS*GRIDPOINTS * 3 );

        oyHash_SetPointer( entry, (oyStruct_s*) clut );

        if(oy_debug >= 2)
        {
          char * fn = 0;
          static int c = 0;
          oyStringAddPrintf( &fn, malloc, free, "dbg-clut-%d.ppm", c);
          oyImage_WritePPM(image_out, fn, hash_text);
          free(fn); fn = 0;
          oyStringAddPrintf( &fn, malloc, free, "dbg-clut-%d.icc", c++);
          FILE*fp=fopen(fn,"w");
          if(fp) fwrite( oyBlob_GetPointer( blob ), sizeof(char), oyBlob_GetSize( blob ), fp );
          if(fp) fclose(fp);
        }
      }

      if(hash_text)
      {
        cicc_free(hash_text); hash_text = 0;
      }


      oyOptions_Release( &options );
      oyImage_Release( &image_in );
      oyImage_Release( &image_out );
      oyConversion_Release( &cc );

      cdCreateTexture( ccontext );

    }

    if(!ccontext->dst_profile)
    {
      oyCompLogMessage( NULL, "compicc", CompLogLevelInfo,
                      DBG_STRING "Output \"%s\": no profile",
                      DBG_ARGS, ccontext->output_name);
    }

    clean_setupColourTable:
    if(web)
      oyProfile_Release( &web );
}

static int     getDisplayAdvanced    ( CompScreen        * s,
                                       int                 screen OY_UNUSED )
{
  CompDisplay * d = s->display;
  PrivDisplay * pd = compObjectGetPrivate((CompObject *) d);
  unsigned long nBytes;
  char * opt = 0;
  int advanced = 0;
  Window root = RootWindow( s->display->display, 0 );

  /* optionally set advanced options from Oyranos */
  opt = fetchProperty( s->display->display, root, pd->iccDisplayAdvanced,
                       XA_STRING, &nBytes, False );
  if(oy_debug)
        printf( DBG_STRING "iccDisplayAdvanced: %s %lu\n",
                DBG_ARGS, opt?opt:"", nBytes);
  if(opt && nBytes && atoi(opt) > 0)
        advanced = atoi(opt);
  if(opt)
  {     XFree( opt ); opt = 0; }

  return advanced;
}

static void    setupOutputTable      ( CompScreen        * s,
                                       oyConfig_s        * device OY_UNUSED,
                                       int                 screen )
{
  PrivScreen *ps = compObjectGetPrivate((CompObject *) s);
  PrivColorOutput * output = &ps->contexts[screen];


  if(!colour_desktop_can)
    return;


  output->cc.src_profile = oyProfile_FromStd( oyASSUMED_WEB, icc_profile_flags, 0 );
  output->cc.output_name = strdup(output->name);
  if(!output->cc.src_profile)
    oyCompLogMessage(s->display, "compicc", CompLogLevelWarn,
             DBG_STRING "Output %s: no oyASSUMED_WEB src_profile",
             DBG_ARGS, output->name );

  setupColourTable( &output->cc, getDisplayAdvanced( s, screen ), s );
}

static void freeOutput( PrivScreen *ps )
{
  if (ps->nContexts > 0)
  {
    for (unsigned long i = 0; i < ps->nContexts; ++i)
    {
      if(ps->contexts[i].cc.dst_profile)
        oyProfile_Release( &ps->contexts[i].cc.dst_profile );
      if(ps->contexts[i].cc.glTexture)
        glDeleteTextures( 1, &ps->contexts[i].cc.glTexture );
      ps->contexts[i].cc.glTexture = 0;
    }
    cicc_free(ps->contexts);
  }
}


void cleanDisplayProfiles( CompScreen *s )
{
    oyOptions_s * opts = 0,
                * result = 0;

    const char * display_name = strdup(XDisplayString(s->display->display));

    oyOptions_SetFromString( &opts, "////display_name",
                           display_name, OY_CREATE_NEW );
    oyOptions_Handle( "//" OY_TYPE_STD "/clean_profiles",
                                opts,"clean_profiles",
                                &result );
    return;
}

/**
 * Called when output configuration (or properties) change.
 */
static void setupOutputs(CompScreen *s)
{
  PrivScreen *ps = compObjectGetPrivate((CompObject *) s);
  int n;

  /* clean memory */
  {
    freeOutput(ps);

    cleanDisplayProfiles( s );
  }

  n = s->nOutputDev;

  {
    int i;
    ps->nContexts = n;
    ps->contexts = (PrivColorOutput*)cicc_alloc( ps->nContexts *
                                             sizeof(PrivColorOutput ));
    for(i = 0; i < n; ++i)
      ps->contexts[i].cc.ref = 1;
  }

  /* allow Oyranos to see modifications made to the compiz Xlib context */
  XFlush( s->display->display );
}

oyConfigs_s * old_devices = NULL;
int            needUpdate            ( Display           * display )
{
  int error = 0,
      i, n, update = 0;
  oyOptions_s * options = 0;
  oyConfigs_s * devices = 0;
  oyConfig_s * device = 0, * old_device = 0;


  /* allow Oyranos to see modifications made to the compiz Xlib context */
  XFlush( display );

  /* obtain device informations, including geometry and ICC profiles
     from the according Oyranos module */
  error = oyOptions_SetFromString( &options, "//" OY_TYPE_STD "/config/command",
                                 "list", OY_CREATE_NEW );
  if(error) fprintf(stdout,"%s %d", "found issues",error);
  error = oyOptions_SetFromString( &options, "//" OY_TYPE_STD "/config/device_rectangle",
                                 "true", OY_CREATE_NEW );
  if(error) fprintf(stdout,"%s %d", "found issues",error);
  error = oyOptions_SetFromString( &options, "//" OY_TYPE_STD "/config/edid",
                                 "refresh", OY_CREATE_NEW );
  error = oyDevicesGet( OY_TYPE_STD, "monitor", options, &devices );
  if(error) fprintf(stdout,"%s %d", "found issues",error);
  n = oyOptions_Count( options );
  oyOptions_Release( &options );

  n = oyConfigs_Count( devices );
  /* find out if monitors have changed at all
   * care only about EDID's and enumeration, no dimension */
  if(n != oyConfigs_Count( old_devices ))
    update = 1;
  else
  for(i = 0; i < n; ++i)
  {
    const char * edid, * old_edid, * rect, * old_rect;
    device = oyConfigs_Get( devices, i );
    old_device = oyConfigs_Get( old_devices, i );
    edid = oyOptions_FindString( *oyConfig_GetOptions(device,"backend_core"),"EDID",0 );
    old_edid = oyOptions_FindString( *oyConfig_GetOptions(old_device,"backend_core"),"EDID",0 );
    rect = oyOptions_FindString( *oyConfig_GetOptions(device,"backend_core"),"display_geometry",0 );
    old_rect = oyOptions_FindString( *oyConfig_GetOptions(old_device,"backend_core"),"display_geometry",0 );

    if(edid && old_edid && strcmp(edid,old_edid)==0 && strcmp(rect,old_rect)==0)
      update = 0;
    else
      update = 1;

    oyConfig_Release( &device );
    oyConfig_Release( &old_device );
    if(update) break;
  }

  oyConfigs_Release( &old_devices );
  old_devices = devices;

  fprintf( stderr,"%s:%d %s() update: %d\n", __FILE__, __LINE__, __func__, update);
  return update;
}

/**
 * Called when XRandR output configuration (or properties) change. Fetch
 * output profiles (if available) or fall back to sRGB.
 * Device profiles are obtained from Oyranos only once at beginning.
 */
static void updateOutputConfiguration(CompScreen *s, CompBool init, int screen)
{
  PrivScreen *ps = compObjectGetPrivate((CompObject *) s);
  int error = 0,
      set = 1;
  oyOptions_s * options = 0;
  oyConfigs_s * devices = 0;
  oyConfig_s * device = 0;

  /* allow Oyranos to see modifications made to the compiz Xlib context */
  XFlush( s->display->display );

  /* reset Oyranos DB cache to see new DB values */
  oyGetPersistentStrings( NULL );
  if(oy_debug)
        printf( DBG_STRING "resetted Oyranos DB cache init: %d screen: %d\n",
                DBG_ARGS, init, screen );


  /* obtain device informations, including geometry and ICC profiles
     from the according Oyranos module */
  error = oyOptions_SetFromString( &options, "//" OY_TYPE_STD "/config/command",
                                 "list", OY_CREATE_NEW );
  error = oyOptions_SetFromString( &options, "//" OY_TYPE_STD "/config/device_rectangle",
                                 "true", OY_CREATE_NEW );
  error = oyDevicesGet( OY_TYPE_STD, "monitor", options, &devices );
  if(error > 0)
          oyCompLogMessage( NULL, "compicc", CompLogLevelWarn,
                      DBG_STRING "oyDevicesGet() error: %d",
                      DBG_ARGS, error);
  oyOptions_Release( &options );

  if(colour_desktop_can && init)
  {
    // set _ICC_COLOR_DESKTOP in advance to handle vcgt correctly
    error = updateIccColorDesktopAtom( s, ps, 2 );
    oyCompLogMessage( NULL, "compicc", CompLogLevelDebug,
                      DBG_STRING "updateIccColorDesktopAtom() status: %d",
                      DBG_ARGS, error);
  }

  if(colour_desktop_can)
  for (unsigned long i = 0; i < ps->nContexts; ++i)
  {
    if( screen >= 0 && (int)i != screen )
      continue;

    device = oyConfigs_Get( devices, i );

    if(init)
    {
      error = getDeviceProfile( s, ps, device, i );
      if(error > 0)
          oyCompLogMessage( NULL, "compicc", CompLogLevelWarn,
                      DBG_STRING "getDeviceProfile() error: %d",
                      DBG_ARGS, error);
    }

    if(ps->contexts[i].cc.dst_profile)
    {
      moveICCprofileAtoms( s, i, set );
    } else
    {
      oyCompLogMessage( s->display, "compicc", CompLogLevelDebug,
                  DBG_STRING "No profile found on desktops %d/%d 0x%lx 0x%lx",
                  DBG_ARGS, i, ps->nContexts, &ps->contexts[i],
                  ps->contexts[i].cc.dst_profile);
    }
    setupOutputTable( s, device, i );

    oyConfig_Release( &device );
  }
  oyConfigs_Release( &devices );

  {
    int all = 1;
    forEachWindowOnScreen( s, damageWindow, &all );
  }
}



/**
 * CompDisplay::handleEvent
 */
static void pluginHandleEvent(CompDisplay *d, XEvent *event)
{
  PrivDisplay *pd = compObjectGetPrivate((CompObject *) d);
  const char * atom_name = 0;

  UNWRAP(pd, d, handleEvent);
  (*d->handleEvent) (d, event);
  WRAP(pd, d, handleEvent, pluginHandleEvent);

  if(!colour_desktop_can)
    return;

  CompScreen * s = findScreenAtDisplay(d, event->xany.window);
  PrivScreen * ps = compObjectGetPrivate((CompObject *) s);


  switch (event->type)
  {
  case PropertyNotify:
    atom_name = XGetAtomName( event->xany.display, event->xproperty.atom );

    if (event->xproperty.atom == pd->iccColorProfiles)
    {
      CompScreen *s = findScreenAtDisplay(d, event->xproperty.window);
      updateScreenProfiles(s);
    } else if (event->xproperty.atom == pd->iccColorRegions)
    {
      CompWindow *w = findWindowAtDisplay(d, event->xproperty.window);
      updateWindowRegions(w);
      colour_desktop_region_count = -1;
    } else if (event->xproperty.atom == pd->iccColorOutputs)
    {
      CompWindow *w = findWindowAtDisplay(d, event->xproperty.window);
      updateWindowOutput(w);

    /* let possibly others take over the colour server */
    } else if( event->xproperty.atom == pd->iccColorDesktop && atom_name )
    {
      updateIccColorDesktopAtom( s, ps, 0 );

    /* update for a changing monitor profile */
    } else if(
           strstr( atom_name, XCM_ICC_V0_3_TARGET_PROFILE_IN_X_BASE) != 0/* &&
           strstr( atom_name, "ICC_PROFILE_IN_X") == 0*/ )
    {
      if(colour_desktop_can)
      {
        int screen = 0;
        int ignore_profile = 0;
        char * icc_colour_server_profile_atom = (char*)cicc_alloc(1024);
        char num[12];
        Atom da;
        unsigned long n = 0;

        if(strlen(atom_name) > strlen(XCM_ICC_V0_3_TARGET_PROFILE_IN_X_BASE"_"))
        sscanf( (const char*)atom_name,
                XCM_ICC_V0_3_TARGET_PROFILE_IN_X_BASE "_%d", &screen );
        snprintf( num, 12, "%d", screen );

        snprintf( icc_colour_server_profile_atom, 1024,
                  XCM_DEVICE_PROFILE"%s%s",
                  screen ? "_" : "", screen ? num : "" );

        da = XInternAtom( d->display, icc_colour_server_profile_atom, False);

        if(da)
        {
          char * data = fetchProperty( d->display, RootWindow(d->display,0),
                                       event->xproperty.atom, XA_CARDINAL,
                                       &n, False);
          if(data && n)
          {
            oyProfile_s * sp = oyProfile_FromMem( n, data, 0,0 ); /* server p */
            oyProfile_s * web = oyProfile_FromStd( oyASSUMED_WEB, icc_profile_flags, 0 );

            /* The distinction of sRGB profiles set by the server and ones
             * coming from outside the colour server is rather fragile.
             * So we ignore any sRGB profiles set into _ICC_PROFILE(_xxx).
             * The correct way to omit colour correction is to tag window
             * regions. As a last resort the colour server can be switched off.
             */
            if(oyProfile_Equal( sp, web ))
            {
              oyProfile_Release( &sp );
              ignore_profile = 1;
            }
            oyProfile_Release( &web );

            if(sp)
            {
              if((int)ps->nContexts > screen)
              {
                oyProfile_Release( &ps->contexts[screen].cc.dst_profile );
                ps->contexts[screen].cc.dst_profile = sp;
              } else
                oyCompLogMessage( s->display, "compicc",CompLogLevelWarn,
                    DBG_STRING "contexts not ready for screen %d / %d",
                    DBG_ARGS, screen, ps->nContexts );
              
              changeProperty ( d->display,
                               da, XA_CARDINAL,
                               (unsigned char*)NULL, 0 );
            }
            sp = 0;
            XFree( data );
          }
        }

        if(icc_colour_server_profile_atom) cicc_free(icc_colour_server_profile_atom);

        if(!ignore_profile &&
           /* change only existing profiles, ignore removed ones */
           n)
        {
          updateOutputConfiguration( s, FALSE, screen );
        }
      }

    /* update for changing geometry */
    } else if (event->xproperty.atom == pd->netDesktopGeometry &&
               needUpdate(s->display->display))
    {
      setupOutputs( s );
      updateOutputConfiguration(s, TRUE, -1);
    } else if (event->xproperty.atom == pd->iccDisplayAdvanced)
    {
      updateOutputConfiguration( s, FALSE, -1 );
    }

    break;
  default:
#ifdef HAVE_XRANDR
    if (event->type == d->randrEvent + RRNotify)
    {
      XRRNotifyEvent *rrn = (XRRNotifyEvent *) event;
      if(rrn->subtype == RRNotify_OutputChange)
      {
        CompScreen *s = findScreenAtDisplay(d, rrn->window);
        if(needUpdate(s->display->display))
        {
          setupOutputs( s );
          updateOutputConfiguration(s, TRUE, -1);
        }
      }
    }
#endif
    break;
  }

  /* initialise */
  if(s && ps && s->nOutputDev != (int)ps->nContexts)
  {
    setupOutputs( s );
    updateOutputConfiguration( s, TRUE, -1);
  }
}

/**
 * Make region relative to the window. 
 * Uses static variables to prevent
 * allocating and freeing the Region in pluginDrawWindow().
 */
static Region absoluteRegion(CompWindow *w, Region region)
{
  Region r = XCreateRegion();
  XUnionRegion( region, r, r );

  for (int i = 0; i < r->numRects; ++i) {
    r->rects[i].x1 += w->attrib.x;
    r->rects[i].x2 += w->attrib.x;
    r->rects[i].y1 += w->attrib.y;
    r->rects[i].y2 += w->attrib.y;

    EXTENTS(&r->rects[i], r);
  }

  return r;
}

static void damageWindow(CompWindow *w, void *closure)
{
  PrivWindow *pw = compObjectGetPrivate((CompObject *) w);
  int * all = closure;

  /* scrissored rects seem to be insensible to artifacts from other windows */
  if((HAS_REGIONS(pw) || (all && *all == 1)) &&
      pw->absoluteWindowRectangleOld /*&&
      (w->type ==1 || w->type == 128) &&
      w->resName*/)
  {
    /* what is so expensive */
    addWindowDamage(w);
  }
}

static void addWindowRegionCount(CompWindow *w, void * var)
{
  PrivWindow *pw = compObjectGetPrivate((CompObject *) w);
  signed long * count = var;
  if(pw)
  {
    if(HAS_REGIONS(pw))
    {
      pw->stencil_id_start = *count;
      *count = *count + pw->nRegions;
    } else
      pw->stencil_id_start = 0;
  }
}

/**
 * CompScreen::drawWindow
 *  The window's texture is mapped on screen.
 *  As this is the second step of drawing a window it is not best suited to
 *  declare a region for colour conversion.
 *  On the other side here can overlapping regions be found to reduce the 
 *  colour transformed area like:
 *  - draw all windows regions into the stencil buffer
 *  - draw all window textures as needed by the flat desktop
 *  - map all windows to the screen
 *  The inclusion of perspective shifts is not reasonably well done.
 */
static Bool pluginDrawWindow(CompWindow *w, const CompTransform *transform, const FragmentAttrib *attrib, Region region, unsigned int mask)
{
  CompScreen *s = w->screen;
  PrivScreen *ps = compObjectGetPrivate((CompObject *) s);
  unsigned long i,j;

  /* check every 10 seconds */
  time_t  cutime;         /* Time since epoch */
  cutime = time(NULL);    /* current user time */
  if((cutime - icc_color_desktop_last_time > (time_t)10))
    updateIccColorDesktopAtom( s, ps, 0 );

  UNWRAP(ps, s, drawWindow);
  Bool status = (*s->drawWindow) (w, transform, attrib, region, mask);
  WRAP(ps, s, drawWindow, pluginDrawWindow);

  /* If no regions have been enabled, just return as we're done */
  PrivWindow *pw = compObjectGetPrivate((CompObject *) w);

  /* initialise window regions */
  if (pw->active == 0)
    updateWindowRegions( w );

  if(colour_desktop_region_count == -1)
  {
    colour_desktop_region_count = 0;
    forEachWindowOnScreen( s, addWindowRegionCount,
                           &colour_desktop_region_count );
  }

  oyRectangle_s * rect = oyRectangle_NewWith( w->serverX, w->serverY,
                                          w->serverWidth, w->serverHeight, 0 );

  /* update to window movements and resizes */
  if( !oyRectangle_IsEqual( rect, pw->absoluteWindowRectangleOld ) )
  {
    forEachWindowOnScreen(s, damageWindow, NULL);

    if(w->serverWidth != oyRectangle_GetGeo1( pw->absoluteWindowRectangleOld, 2 ) ||
       w->serverHeight != oyRectangle_GetGeo1( pw->absoluteWindowRectangleOld, 3 ))
      updateWindowRegions( w );

    oyRectangle_SetByRectangle( pw->absoluteWindowRectangleOld, rect );

  }

  oyRectangle_Release( &rect );

  /* skip the stencil drawing for to be scissored windows */
  if( !HAS_REGIONS(pw) )
    return status;

  int use_stencil_test = glIsEnabled(GL_STENCIL_TEST);
  glEnable(GL_STENCIL_TEST);

  /* Replace the stencil value in places where we'd draw something */
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

  /* Disable color mask as we won't want to draw anything */
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

  for( j = 0; j < pw->nRegions; ++j )
  {
    PrivColorRegion * window_region = pw->pRegion + j;
    Region aRegion = absoluteRegion( w, window_region->xRegion);

    for( i = 0; i < ps->nContexts; ++i )
    {
      /* Each region gets its own stencil value */
      glStencilFunc(GL_ALWAYS, STENCIL_ID, ~0);

      /* intersect window with monitor */
      Region screen = XCreateRegion();
      XUnionRectWithRegion( &ps->contexts[i].xRect, screen, screen );    
      Region intersection = XCreateRegion();
      XIntersectRegion( screen, aRegion, intersection );
      BOX * b = &intersection->extents;
      if(b->x1 == 0 && b->x2 == 0 && b->y1 == 0 && b->y2 == 0)
        goto cleanDrawWindow;

      if(oy_debug >= 3)
      fprintf( stderr, DBG_STRING"STENCIL_ID = %lu (1 + colour_desktop_region_count=%ld * i=%lu + pw->stencil_id_start=%lu + j=%lu)\n", DBG_ARGS,
               STENCIL_ID,colour_desktop_region_count,i,pw->stencil_id_start,j);

      w->vCount = w->indexCount = 0;
      (*w->screen->addWindowGeometry) (w, &w->matrix, 1, intersection, region);

      /* If the geometry is non-empty, draw the window */
      if (w->vCount > 0)
      {
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        (*w->drawWindowGeometry) (w);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
      }

      cleanDrawWindow:
      XDestroyRegion( intersection );
      XDestroyRegion( screen );
    }

    XDestroyRegion( aRegion ); aRegion = 0;
  }

  /* Reset the color mask */
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  if(use_stencil_test)
    glEnable(GL_STENCIL_TEST);
  else
    glDisable(GL_STENCIL_TEST);

  return status;
}

/**
 * CompScreen::drawWindowTexture
 *  The window's texture or content is drawn here.
 *  Where does we know which monitor we draw on? from
 *  - pluginDrawWindow()
 *  - Oyranos
 */
static void pluginDrawWindowTexture(CompWindow *w, CompTexture *texture, const FragmentAttrib *attrib, unsigned int mask)
{
  CompScreen *s = w->screen;
  PrivScreen *ps = compObjectGetPrivate((CompObject *) s);

  UNWRAP(ps, s, drawWindowTexture);
  (*s->drawWindowTexture) (w, texture, attrib, mask);
  WRAP(ps, s, drawWindowTexture, pluginDrawWindowTexture);

  PrivWindow *pw = compObjectGetPrivate((CompObject *) w);
  if (pw->active == 0)
    return;

  /* Set up the shader */
  FragmentAttrib fa = *attrib;

  int param = allocFragmentParameters(&fa, 2);
  int unit = allocFragmentTextureUnits(&fa, 1);
  int function = getProfileShader(s, texture, param, unit);
  if (function)
    addFragmentFunction(&fa, function);

  int use_stencil_test = glIsEnabled(GL_STENCIL_TEST);
  int use_scissor_test = glIsEnabled(GL_SCISSOR_TEST);

  if( HAS_REGIONS(pw) )
  {
    glEnable(GL_STENCIL_TEST);
    glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
  } else
    glEnable(GL_SCISSOR_TEST);

  unsigned long i, j = 0;
  for(i = 0; i < ps->nContexts; ++i)
  {
    Region tmp = 0;
    Region screen = 0;
    Region intersection = 0;
    /* draw the texture over the whole monitor to affect wobbly windows */
    XRectangle * r = &ps->contexts[i].xRect;
    oyRectangle_s * scissor_box = oyRectangle_NewWith( r->x, s->height - r->y - r->height, r->width, r->height, NULL );
    /* honour the previous scissor rectangle */
    oyRectangle_s * scissor = oyRectangle_NewFrom( scissor_box, NULL );
    GLint box[4] = {-1,-1,-1,-1};
    glGetIntegerv( GL_SCISSOR_BOX, &box[0] );
    oyRectangle_s * global_box = oyRectangle_NewWith( box[0], box[1], box[2], box[3], NULL );
    oyRectangle_Trim( scissor, global_box );
    if(oy_debug)
    {
      char * gb = strdup( oyRectangle_Show( global_box ) ),
           * sb = strdup( oyRectangle_Show( scissor_box ) );
      if(!oyRectangle_IsEqual( scissor_box, scissor ))
        printf("%lu GL_SCISSOR_BOX: %s scissor: %s trimmed: %s\n",
               i, gb, sb, oyRectangle_Show( scissor ));
      free(gb); free(sb);
    }
    oyRectangle_Release( &global_box );
    if(ps->nContexts > 1)
      glScissor( oyRectangle_GetGeo1(scissor_box, 0),
                 oyRectangle_GetGeo1(scissor_box, 1),
                 oyRectangle_GetGeo1(scissor_box, 2),
                 oyRectangle_GetGeo1(scissor_box, 3) );
    oyRectangle_Release( &scissor_box );
    oyRectangle_Release( &scissor );

    if(WINDOW_INVISIBLE(w))
      goto cleanDrawTexture;

    for( j = 0; j < pw->nRegions; ++j )
    {
      /* get the window region to find zero sized ones */
      PrivColorRegion * window_region = pw->pRegion + j;
      tmp = absoluteRegion( w, window_region->xRegion);
      screen = XCreateRegion();
      XUnionRectWithRegion( &ps->contexts[i].xRect, screen, screen );    
      intersection = XCreateRegion();

      /* create intersection of window and monitor */
      XIntersectRegion( screen, tmp, intersection );

      /* Only draw where the stencil value matches the window and output */
      glStencilFunc(GL_EQUAL, STENCIL_ID, ~0);

      PrivColorContext * c = NULL;
      if(window_region->cc)
        c = window_region->cc[i];

      /* set last region, which is the window region, to default colour table */
      if(j == pw->nRegions - 1)
      {
        c = &ps->contexts[i].cc;
        if(!c)
          oyCompLogMessage( s->display, "compicc", CompLogLevelWarn,
                    DBG_STRING "No CLUT found for screen %d / %d / %lu",
                    DBG_ARGS, screen, ps->nContexts, j );

        /* test for stencil capabilities to place region ID */
        GLint stencilBits = 0;
        glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
        if(stencilBits == 0 && pw->nRegions > 1)
          c = NULL;
      }

      BOX * b = &intersection->extents;

      if(oy_debug >= 3 && pw->nRegions != 1)
        fprintf( stderr, DBG_STRING"STENCIL_ID = %lu (1 + colour_desktop_region_count=%lu * i=%lu + pw->stencil_id_start=%lu + j=%lu) pw->nRegions=%lu glTexture=%u\t%d,%d,%dx%d\n", DBG_ARGS,
               STENCIL_ID,colour_desktop_region_count,i,pw->stencil_id_start,j,
               pw->nRegions, c?c->glTexture:0, b->x1, b->y1, b->x2-b->x1, b->y2-b->y1 );

      if(!c ||
         (b->x1 == 0 && b->x2 == 0 && b->y1 == 0 && b->y2 == 0))
        goto cleanDrawTexture;

      if(0 && oy_debug)
        fprintf( stderr, DBG_STRING"i=%lu j=%lu glTexture=%d\t%s -> %s %s \t%d,%d,%dx%d\n", DBG_ARGS,
               i, j, c->glTexture,
               oyProfile_GetFileName( c->src_profile, -1 ),
               oyProfile_GetText( c->dst_profile, oyNAME_DESCRIPTION ),
               c->output_name, b->x1, b->y1, b->x2-b->x1, b->y2-b->y1 );

      /* Set the environment variables */
      glProgramEnvParameter4dARB( GL_FRAGMENT_PROGRAM_ARB, param + 0,
                                c->scale, c->scale, c->scale, 1.0);
      glProgramEnvParameter4dARB( GL_FRAGMENT_PROGRAM_ARB, param + 1,
                                c->offset, c->offset, c->offset, 0.0);

      if(c->glTexture)
      {
        /* Activate the 3D texture */
        (*s->activeTexture) (GL_TEXTURE0_ARB + unit);
        glEnable(GL_TEXTURE_3D);
        glBindTexture(GL_TEXTURE_3D, c->glTexture);
        (*s->activeTexture) (GL_TEXTURE0_ARB);
      }

      /* Now draw the window texture */
      UNWRAP(ps, s, drawWindowTexture);
      if(c->glTexture)
        (*s->drawWindowTexture) (w, texture, &fa, mask);
      else
        /* ignore the shader */
        (*s->drawWindowTexture) (w, texture, attrib, mask);
      WRAP(ps, s, drawWindowTexture, pluginDrawWindowTexture);

      if(c->glTexture)
      {
        /* Deactivate the 3D texture */
        (*s->activeTexture) (GL_TEXTURE0_ARB + unit);
        glBindTexture(GL_TEXTURE_3D, 0);
        glDisable(GL_TEXTURE_3D);
        (*s->activeTexture) (GL_TEXTURE0_ARB);
      }

      cleanDrawTexture:
      if(intersection)
        XDestroyRegion( intersection );
      if(tmp)
        XDestroyRegion( tmp );
      if(screen)
        XDestroyRegion( screen );
    }
    if(ps->nContexts > 1)
      glScissor( box[0], box[1], box[2], box[3] );
  }

  if(use_stencil_test)
    glEnable(GL_STENCIL_TEST);
  else
    glDisable(GL_STENCIL_TEST);
  if(use_scissor_test)
    glEnable(GL_SCISSOR_TEST);
  else
    glDisable(GL_SCISSOR_TEST);
}


/**
 *    Object Init Functions
 */

static CompBool pluginInitCore(CompPlugin *plugin OY_UNUSED, CompObject *object OY_UNUSED, void *privateData OY_UNUSED)
{
#if defined(PLUGIN_DEBUG_)
  int dbg_switch = 60;

  while(dbg_switch--)
    sleep(1);
#endif

  /* select profiles matching actual capabilities */
  icc_profile_flags = oyICCProfileSelectionFlagsFromOptions( OY_CMM_STD, "//" OY_TYPE_STD "/icc_color", NULL, 0 );

  return TRUE;
}

/**
 *  Check and update the _ICC_COLOR_DESKTOP status atom. It is used to 
 *  communicate to the colour server.
 *
 *  The _ICC_COLOR_DESKTOP atom is a string with following usages:
 *  - uniquely identify the colour server
 *  - tell the name of the colour server
 *  - tell the colour server is alive
 *  All sections are separated by one space char ' ' for easy parsing.
 *  The first section contains the pid_t of the process which has set the atom.
 *  The second section contains time since epoch GMT as returned by time(NULL).
 *  The thired section contains the bar '|' separated and surrounded
 *  capabilities:
 *    - ICP  _ICC_COLOR_PROFILES  - support per region profiles
 *    - ICM  _ICC_COLOR_MANAGEMENT - color server is active
 *    - ICR  _ICC_COLOR_REGIONS - support regions
 *    - ICA  _ICC_COLOR_DISPLAY_ADVANCED - use CMS advanced settings, e.g. proofing
 *    - _ICC_COLOR_DESKTOP is omitted
 *    - V0.3 indicates version compliance to the _ICC_Profile in X spec
 *  The fourth section contains the server name identifier.
 *
 * @param[in]      request             - 0  update
 *                                     - 2  init
 * @return                             - 0  all fine
 *                                     - 1  inactivate
 *                                     - 2  activate
 *                                     - 3  error
 */
static int updateIccColorDesktopAtom ( CompScreen        * s,
                                       PrivScreen        * ps,
                                       int                 request )
{
  CompDisplay * d = s->display;
  PrivDisplay * pd = compObjectGetPrivate((CompObject *) d);
  time_t  cutime;         /* Time since epoch */
  cutime = time(NULL);    /* current user time */
  const char * my_id = "compicc",
             * my_capabilities = "|ICM|ICP|ICR|ICA|V0.3|"; /* _ICC_COLOR_REGIONS
                                                    * _ICC_COLOR_PROFILES */
  unsigned long n = 0;
  char * data = 0;
  const char * old_atom = 0;
  int status = 0;
 

  /* set the colour management desktop service activity atom */
  pid_t pid = getpid();
  int old_pid = 0;
  long atom_time = 0;
  char * atom_colour_server_name,
       * atom_capabilities_text;

  if(!colour_desktop_can)
    return 1;

  atom_colour_server_name = (char*)cicc_alloc(1024);
  atom_capabilities_text = (char*)cicc_alloc(1024);
  if(!atom_colour_server_name || !atom_capabilities_text)
  {
    status = 3;
    goto clean_updateIccColorDesktopAtom;
  }

  atom_colour_server_name[0] = atom_capabilities_text[0] = '\000';

  data = fetchProperty( d->display, RootWindow(d->display,0),
                        pd->iccColorDesktop, XA_STRING, &n, False);

  atom_colour_server_name[0] = 0;
  if(n && data && strlen(data))
  {
    sscanf( (const char*)data, "%d %ld %s %s",
            &old_pid, &atom_time,
            atom_capabilities_text, atom_colour_server_name );
    old_atom = data;
  }

  if(n && data && old_pid != (int)pid)
  {
    if(old_atom && atom_time + 60 < cutime)
      oyCompLogMessage( d, "compicc", CompLogLevelWarn,
                    DBG_STRING "\n!!! Found old _ICC_COLOR_DESKTOP pid: %s.\n"
                    "Eigther there was a previous crash or your setup can be double colour corrected.",
                    DBG_ARGS, old_atom ? old_atom : "????" );
    /* check for taking over of colour service */
    if(atom_colour_server_name && strcmp(atom_colour_server_name, my_id) != 0)
    {
      if( atom_time < icc_color_desktop_last_time ||
          /* check for the only other known color server; it can only run for KWin */
          ( atom_colour_server_name &&
            strcmp(atom_colour_server_name, "kolorserver") == 0 ) ||
          request == 2  )
      {
        oyCompLogMessage( d, "compicc", CompLogLevelWarn,
                    DBG_STRING "\nTaking over colour service from old _ICC_COLOR_DESKTOP: %s.",
                    DBG_ARGS, old_atom ? old_atom : "????" );

        fetchProperty( d->display, RootWindow(d->display,0),
                        pd->iccColorDesktop, XA_STRING, &n, True);

      } else
      if(atom_time > icc_color_desktop_last_time)
      {
        oyCompLogMessage( d, "compicc", CompLogLevelWarn,
                    DBG_STRING "\nGiving colour service to _ICC_COLOR_DESKTOP: %s.",
                    DBG_ARGS, old_atom ? old_atom : "????" );
     
        colour_desktop_can = 0;
      }
    } else
    if(old_atom)
      oyCompLogMessage( d, "compicc", CompLogLevelWarn,
                    DBG_STRING "\nTaking over colour service from old _ICC_COLOR_DESKTOP: %s.",
                    DBG_ARGS, old_atom ? old_atom : "????" );
  }

  /*  Do we really colour correct?
   *  This is only a guess.
   */
  int attached_profiles = 0;
  for(unsigned long i = 0; i < ps->nContexts; ++i)
    attached_profiles += ps->contexts[i].cc.dst_profile ? 1:0;

  int transform_n = 0;
  for(unsigned long i = 0; i < ps->nContexts; ++i)
    transform_n += ps->contexts[i].cc.glTexture ? 1:0;

  /* test for stencil capabilities to place region ID */
  GLint stencilBits = 0;
  glGetIntegerv(GL_STENCIL_BITS, &stencilBits);

  if( (atom_time + 10) < icc_color_desktop_last_time ||
      request == 2 )
  {
    char * atom_text = (char*)cicc_alloc(1024);
    if(!atom_text) goto clean_updateIccColorDesktopAtom;
    sprintf( atom_text, "%d %ld %s %s",
             (int)pid, (long)cutime,
             /* say if we can convert, otherwise give only the version number */
             transform_n ? (stencilBits?my_capabilities:"|ICM|ICR|ICA|V0.3|"):"|V0.3|",
             my_id );
 
   if(attached_profiles || request == 2)
      changeProperty( d->display,
                                pd->iccColorDesktop, XA_STRING,
                                (unsigned char*)atom_text,
                                strlen(atom_text) + 1 );
    else if(old_atom)
    {
      /* switch off the plugin */
      changeProperty( d->display,
                                pd->iccColorDesktop, XA_STRING,
                                (unsigned char*)NULL, 0 );
      colour_desktop_can = 0;
    }

    if(oy_debug)
    {
      data = fetchProperty( d->display, RootWindow(d->display,0),
                        pd->iccColorDesktop, XA_STRING, &n, False);

      oyCompLogMessage( d, "compicc", CompLogLevelDebug,
                    DBG_STRING "request=%d Set _ICC_COLOR_DESKTOP: %s.",
                    DBG_ARGS, request, data ? data : "????" );
    }

    if(atom_text) cicc_free( atom_text );
  }

clean_updateIccColorDesktopAtom:
  if(atom_colour_server_name) cicc_free(atom_colour_server_name);
  if(atom_capabilities_text) cicc_free(atom_capabilities_text);

  icc_color_desktop_last_time = cutime;

  if(colour_desktop_can == 0)
    for (unsigned long i = 0; i < ps->nContexts; ++i)
    {
      if(ps->contexts[i].cc.glTexture)
        glDeleteTextures( 1, &ps->contexts[i].cc.glTexture );
      ps->contexts[i].cc.glTexture = 0;
    }

  return status;
}

static CompBool pluginInitDisplay(CompPlugin *plugin OY_UNUSED, CompObject *object, void *privateData)
{
  CompDisplay *d = (CompDisplay *) object;
  PrivDisplay *pd = privateData;

  if (d->randrExtension == False)
    return FALSE;

  WRAP(pd, d, handleEvent, pluginHandleEvent);

  pd->iccColorProfiles = XInternAtom(d->display, XCM_COLOR_PROFILES, False);
  pd->iccColorRegions = XInternAtom(d->display, XCM_COLOR_REGIONS, False);
  pd->iccColorOutputs = XInternAtom(d->display, XCM_COLOR_OUTPUTS, False);
  pd->iccColorDesktop = XInternAtom(d->display, XCM_COLOR_DESKTOP, False);
  pd->netDesktopGeometry = XInternAtom(d->display, "_NET_DESKTOP_GEOMETRY", False);
  pd->iccDisplayAdvanced = XInternAtom(d->display, XCM_COLOUR_DESKTOP_ADVANCED, False);

  return TRUE;
}


static CompBool pluginInitScreen(CompPlugin *plugin OY_UNUSED, CompObject *object, void *privateData)
{
  CompScreen *s = (CompScreen *) object;
  PrivScreen *ps = privateData;
#ifdef HAVE_XRANDR
  int screen = DefaultScreen( s->display->display );
#endif
  fprintf( stderr, DBG_STRING"dev %d contexts %ld \n", DBG_ARGS,
          s->nOutputDev, ps->nContexts );
    
  GLint stencilBits = 0;
  glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
  if (stencilBits == 0)
  {
    fprintf( stderr, DBG_STRING"stencilBits %d -> limited profile support (ICP)\n", DBG_ARGS,
             stencilBits );
  }

  WRAP(ps, s, drawWindow, pluginDrawWindow);
  WRAP(ps, s, drawWindowTexture, pluginDrawWindowTexture);

  ps->function = 0;
  ps->function_2 = 0;
  ps->param = ps->param_2 = -1;
  ps->unit = ps->unit_2 = -1;

  /* XRandR setup code */

#ifdef HAVE_XRANDR
  XRRSelectInput( s->display->display,
                  XRootWindow( s->display->display, screen ),
                  RROutputPropertyNotifyMask | RRCrtcChangeNotifyMask |
                  RROutputChangeNotifyMask | RROutputPropertyNotifyMask );
#endif

  /* initialisation is done in pluginHandleEvent() by checking ps->nContexts */
  ps->nContexts = 0;

  return TRUE;
}

static CompBool pluginInitWindow(CompPlugin *plugin OY_UNUSED, CompObject *object OY_UNUSED, void *privateData)
{
  /* CompWindow *w = (CompWindow *) object; */
  PrivWindow *pw = privateData;

  pw->nRegions = 0;
  pw->pRegion = 0;
  pw->active = 0;

  pw->absoluteWindowRectangleOld = 0;
  pw->output = NULL;

  return TRUE;
}

static dispatchObjectProc dispatchInitObject[] = {
  pluginInitCore, pluginInitDisplay, pluginInitScreen, pluginInitWindow
};

/**
 *    Object Fini Functions
 */


static CompBool pluginFiniCore(CompPlugin *plugin OY_UNUSED, CompObject *object OY_UNUSED, void *privateData OY_UNUSED)
{
  return TRUE;
}

static CompBool pluginFiniDisplay(CompPlugin *plugin OY_UNUSED, CompObject *object, void *privateData)
{
  CompDisplay *d = (CompDisplay *) object;
  PrivDisplay *pd = privateData;

  UNWRAP(pd, d, handleEvent);

  return TRUE;
}

static CompBool pluginFiniScreen(CompPlugin *plugin OY_UNUSED, CompObject *object, void *privateData OY_UNUSED)
{
  CompScreen *s = (CompScreen *) object;
  PrivScreen *ps = privateData;

  int error = 0,
      init = 0;
  oyConfigs_s * devices = 0;
  oyConfig_s * device = 0;
  Atom iccColorDesktop = XInternAtom(s->display->display, XCM_COLOR_DESKTOP, False);

  /* remove desktop colour management service mark */
  changeProperty( s->display->display,
                                iccColorDesktop, XA_STRING,
                                (unsigned char*)NULL, 0 );
  XFlush( s->display->display );

  error = oyDevicesGet( OY_TYPE_STD, "monitor", 0, &devices );
  if(error > 0)
          oyCompLogMessage( NULL, "compicc", CompLogLevelWarn,
                      DBG_STRING "oyDevicesGet() error: %d",
                      DBG_ARGS, error);

  /* switch profile atoms back */
  for(unsigned long i = 0; i < ps->nContexts; ++i)
  {
    device = oyConfigs_Get( devices, i );

    if(ps->contexts[i].cc.dst_profile)
      moveICCprofileAtoms( s, i, init );

    oyConfig_Release( &device );
  }
  oyConfigs_Release( &devices );


  /* clean memory */
  freeOutput(ps);

  UNWRAP(ps, s, drawWindow);
  UNWRAP(ps, s, drawWindowTexture);

  return TRUE;
}

static CompBool pluginFiniWindow(CompPlugin *plugin OY_UNUSED, CompObject *object OY_UNUSED, void *privateData OY_UNUSED)
{
  return TRUE;
}

static dispatchObjectProc dispatchFiniObject[] = {
  pluginFiniCore, pluginFiniDisplay, pluginFiniScreen, pluginFiniWindow
};


/**
 *    Plugin Interface
 */
static CompBool pluginInit(CompPlugin *p OY_UNUSED)
{
  const char * od = getenv("OY_DEBUG");
  if(od && od[0]) oy_debug = atoi(od);
  oyMessageFunc_p( oyMSG_DBG, NULL, DBG_STRING, DBG_ARGS );
  return TRUE;
}

static oyStructList_s * privates_cache = 0;
static oyStructList_s * pluginGetPrivatesCache ()
{
  if(!privates_cache)
    privates_cache = oyStructList_New( 0 );
  return privates_cache;
}


oyPointer pluginAllocatePrivatePointer( CompObject * o )
{
  oyPointer ptr = 0;
  int index = -1;
  size_t size = 0;
  static const int privateSizes[] = {
  sizeof(PrivCore), sizeof(PrivDisplay), sizeof(PrivScreen), sizeof(PrivWindow)
  };


  if(!o)
    return 0;
  switch(o->type)
  {
    case COMP_OBJECT_TYPE_CORE:
           if(core_priv_index == -1)
             core_priv_index = allocateCorePrivateIndex( );
           index = core_priv_index;
           size = privateSizes[o->type];
         break;
    case COMP_OBJECT_TYPE_DISPLAY:
           if(display_priv_index == -1)
             display_priv_index = allocateDisplayPrivateIndex( );
           index = display_priv_index;
           size = privateSizes[o->type];
         break;
    case COMP_OBJECT_TYPE_SCREEN:
         {
           CompScreen * s = (CompScreen*)o;
           if(screen_priv_index == -1)
             screen_priv_index = allocateScreenPrivateIndex( s->display );
           index = screen_priv_index;
           size = privateSizes[o->type];
         }
         break;
    case COMP_OBJECT_TYPE_WINDOW:
         {
           CompWindow * w = (CompWindow*)o;
           if(window_priv_index == -1)
             window_priv_index = allocateWindowPrivateIndex( w->screen );
           index = window_priv_index;
           size = privateSizes[o->type];
         }
         break;
  }

  if(index < 0)
    return 0;

  {
    o->privates[index].ptr = cicc_alloc(size);
    if(!o->privates[index].ptr) return 0;
    memset( o->privates[index].ptr, 0, size);
  }

  ptr = o->privates[index].ptr;

  return ptr;
}

oyPointer pluginGetPrivatePointer( CompObject * o )
{
  oyPointer ptr = 0;

  if(!o)
    return 0;
  int index = -1;
  switch(o->type)
  {
    case COMP_OBJECT_TYPE_CORE:
           index = core_priv_index;
         break;
    case COMP_OBJECT_TYPE_DISPLAY:
           index = display_priv_index;
         break;
    case COMP_OBJECT_TYPE_SCREEN:
           index = screen_priv_index;
         break;
    case COMP_OBJECT_TYPE_WINDOW:
           index = window_priv_index;
         break;
  }

  if(index < 0)
    return 0;

  ptr = o->privates[index].ptr;
  if(!ptr)
    fprintf( stderr, "object[0x%lx] type=%d no private data reserved\n",
            (intptr_t)o, o->type );

  return ptr;
}

static CompBool pluginInitObject(CompPlugin *p, CompObject *o)
{
  /* use Oyranos for caching of private data */
  oyPointer private_data = pluginAllocatePrivatePointer( o );

  if (dispatchInitObject[o->type](p, o, private_data) == FALSE)
    return FALSE;
  return TRUE;
}

static void pluginFiniObject(CompPlugin *p, CompObject *o)
{
  void *privateData = compObjectGetPrivate(o);
  if (privateData == NULL)
    return;

  dispatchFiniObject[o->type](p, o, privateData);
  if(!o)
    return;

  /* release a cache entry of private data */
  compObjectFreePrivate( o );
}

static void pluginFini(CompPlugin *p OY_UNUSED)
{
  oyStructList_Release( &privates_cache );
}

static CompMetadata *pluginGetMetadata(CompPlugin *p OY_UNUSED)
{
  return &pluginMetadata;
}

CompPluginVTable pluginVTable = {
  "compicc",
  pluginGetMetadata,
  pluginInit,
  pluginFini,
  pluginInitObject,
  pluginFiniObject,
  0,
  0
};

CompPluginVTable *getCompPluginInfo20070830(void)
{
  return &pluginVTable;
}

