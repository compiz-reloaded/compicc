/**
 *  @file     compicc.c
 *
 *  @brief    a compiz desktop colour management plug-in
 *
 *  @author   Kai-Uwe Behrmann, based on Tomas' color filter, based on Gerhard
 *            Fürnkranz' GLSL ppm_viewer
 *  @par Copyright:
 *            2008 (C) Gerhard Fürnkranz, 2008 (C) Tomas Carnecky,
              2009-2011 (C) Kai-Uwe Behrmann
 *  @par License:
 *            new BSD <http://www.opensource.org/licenses/bsd-license.php>
 *  @since    2009/02/23
 */


#include <assert.h>
#include <math.h>     // floor()
#include <string.h>   // http://www.opengroup.org/onlinepubs/009695399/functions/strdup.html
#include <sys/time.h>
#include <time.h>
#include <unistd.h>   // getpid()

#include <stdarg.h>
#include <icc34.h>

#define GL_GLEXT_PROTOTYPES
#define _BSD_SOURCE

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <X11/extensions/Xfixes.h>

#define HAVE_XRANDR
#ifdef HAVE_XRANDR
#include <X11/extensions/Xrandr.h>
#endif

#include <compiz-common.h>

#include <alpha/oyranos_alpha.h>
#include <alpha/oyranos_cmm.h> // oyCMMptr_New
#include <oyranos_definitions.h> /* ICC Profile in X */

#include <X11/Xcm/Xcm.h>
#include <X11/Xcm/XcmEvents.h>


#define OY_COMPIZ_VERSION (COMPIZ_VERSION_MAJOR * 10000 + COMPIZ_VERSION_MINOR * 100 + COMPIZ_VERSION_MICRO)
#if OY_COMPIZ_VERSION < 708
#define oyCompLogMessage(disp_, plug_in_name, debug_level, format_, ... ) \
        compLogMessage( disp_, plug_in_name, debug_level, format_, __VA_ARGS__ )
#else
#define oyCompLogMessage(disp_, plug_in_name, debug_level, format_, ... ) \
        compLogMessage( plug_in_name, debug_level, format_, __VA_ARGS__ )
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
#define STENCIL_ID ( 1 + colour_desktop_region_count * (ps->nContexts + i) + pw->stencil_id_start + j )

#define HAS_REGIONS(pw) (pw->nRegions > 1)

#define WINDOW_BORDER 30

#if defined(PLUGIN_DEBUG)
#define DBG  printf("%s:%d %s() %.02f\n", DBG_ARGS);
#else
#define DBG
#endif

#define DBG_STRING "\n  %s:%d %s() %.02f "
#define DBG_ARGS (strrchr(__FILE__,'/') ? strrchr(__FILE__,'/')+1 : __FILE__),__LINE__,__func__,(double)clock()/CLOCKS_PER_SEC
#if defined(PLUGIN_DEBUG)
#define START_CLOCK(text) fprintf( stderr, DBG_STRING text " - ", DBG_ARGS );
#define END_CLOCK         fprintf( stderr, "%.02f\n", (double)clock()/CLOCKS_PER_SEC );
#else
#define START_CLOCK(text)
#define END_CLOCK
#endif



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
 * The XserverRegion is dereferenced only when the client sends a
 * _ICC_COLOR_MANAGEMENT ClientMessage to its window. This allows clients to
 * change the region as the window is resized and later send _N_C_M to tell the
 * plugin to re-fetch the region from the server.
 * The profile is resolved as soon as the client uploads the regions into the
 * window. That means clients need to upload profiles first and then the 
 * regions. Otherwise the plugin won't be able to find the profile and no color
 * transformation will be done.
 */
typedef struct {
  /* These members are only valid when this region is part of the
   * active stack range. */
  uint8_t md5[16];
  PrivColorContext * cc;
  Region xRegion;
} PrivColorRegion;

/**
 * Output profiles are currently only fetched using XRandR. For backwards 
 * compatibility the code should fall back to root window properties 
 * (OY_ICC_V0_3_TARGET_PROFILE_IN_X_BASE).
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

  /* ClientMessage sent by the application */
  Atom iccColorManagement;

  /* Window properties */
  Atom iccColorProfiles;
  Atom iccColorRegions;
  Atom iccColorTarget;
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
static int updateNetColorDesktopAtom ( CompScreen        * s,
                                       PrivScreen        * ps,
                                       int                 request );
static int     hasScreenProfile      ( CompScreen        * s,
                                       int                 screen,
                                       int                 server );
static void    moveICCprofileAtoms   ( CompScreen        * s,
                                       int                 screen,
                                       int                 init );
void           cleanDisplayProfiles  ( CompScreen        * s );
static int     cleanScreenProfile    ( CompScreen        * s,
                                       int                 screen,
                                       int                 server );
void           cleanDisplay          ( Display           * display );
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
                                       int                 advanced );
static void changeProperty           ( Display           * display,
                                       Atom                target_atom,
                                       int                 type,
                                       void              * data,
                                       unsigned long       size );
static void *fetchProperty(Display *dpy, Window w, Atom prop, Atom type, unsigned long *n, Bool delete);
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
    free(ptr);
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

	for (XcolorProfile *ptr = data; (intptr_t) ptr < (intptr_t)data + nBytes; ptr = XcolorProfileNext(ptr))
		++count;

	return count;
}

static inline XcolorRegion *XcolorRegionNext(XcolorRegion *region)
{
  unsigned char *ptr = (unsigned char *) region;
  return (XcolorRegion *) (ptr + sizeof(XcolorRegion));
}

static inline unsigned long XcolorRegionCount(void *data, unsigned long nBytes)
{
  return nBytes / sizeof(XcolorRegion);
}

/**
 * Helper function to convert a MD5 into a readable string.
 */
static const char *md5string(const uint8_t md5[16])
{
	static char buffer[33];
	const uint32_t * h = (const uint32_t*)md5;

	buffer[0] = 0;
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

  /* demultiply alpha */
  addDataOpToFunctionData(data, "MUL output.rgb, output.a, output;");
  addDataOpToFunctionData(data, "MUL temp.a, output.a, output.a;");

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
static void *fetchProperty(Display *dpy, Window w, Atom prop, Atom type, unsigned long *n, Bool delete)
{
  Atom actual;
  int format;
  unsigned long left;
  unsigned char *data;

  XFlush( dpy );

  int result = XGetWindowProperty(dpy, w, prop, 0, ~0, delete, type, &actual, &format, n, &left, &data);
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
    entry = oyCacheListGetEntry_( cache, exact_hash_size, hash_text );
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
  oyHash_s * entry;
  oyProfile_s * prof = NULL;
  oyStructList_s * cache = pluginGetPrivatesCache();

  /* Copy the profiles into the array, and create the Oyranos handles. */
  const char * hash_text = md5string(md5);
  entry = oyCacheListGetEntry_( cache, exact_hash_size, hash_text );
  prof = (oyProfile_s *) oyHash_GetPointer( entry, oyOBJECT_PROFILE_S);

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
  }
  if (pw->nRegions)
    free(pw->pRegion);
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

  pw->pRegion = (PrivColorRegion*) calloc(count,sizeof(PrivColorRegion));
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
      pw->pRegion[i].cc = (PrivColorContext*)calloc(1,sizeof(PrivColorContext));

      if(!pw->pRegion[i].cc)
      {
        printf( DBG_STRING "region %lu not ready. Stop!\n",
                DBG_ARGS, i );
        goto out;
      }

      if(ps && ps->nContexts > 0)
      {
        pw->pRegion[i].cc->dst_profile = oyProfile_Copy(
                                           ps->contexts[0].cc.dst_profile, 0 );

        if(!pw->pRegion[i].cc->dst_profile)
        {
          printf( DBG_STRING "output 0 not ready\n",
                  DBG_ARGS );
          continue;
        }
        pw->pRegion[i].cc->src_profile = profileFromMD5(region->md5);

        pw->pRegion[i].cc->output_name = strdup(ps->contexts[0].cc.output_name);
      } else
        printf( DBG_STRING "output_name: %s\n",
                DBG_ARGS, ps->contexts[0].cc.output_name);

      if(pw->pRegion[i].cc->src_profile)
        setupColourTable( pw->pRegion[i].cc, getDisplayAdvanced(w->screen, 0) );
      else
        printf( DBG_STRING "region %lu has no source profile!\n",
                DBG_ARGS, i );
    }

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
  pw->output = fetchProperty(d->display, w->id, pd->iccColorTarget, XA_STRING, &nBytes, False);

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

    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glTexImage3D( GL_TEXTURE_3D, 0, GL_RGB16, GRIDPOINTS,GRIDPOINTS,GRIDPOINTS,
                  0, GL_RGB, GL_UNSIGNED_SHORT, ccontext->clut);
}

static int     hasScreenProfile      ( CompScreen        * s,
                                       int                 screen,
                                       int                 server )
{
  char num[12];
  Window root = RootWindow( s->display->display, 0 );
  char * icc_profile_atom = (char*)calloc( 1024, sizeof(char) );
  Atom a;
  oyPointer data;
  unsigned long n = 0;

  if(!icc_profile_atom) return 0;

  snprintf( num, 12, "%d", (int)screen );
  if(server)
  snprintf( icc_profile_atom, 1024, OY_ICC_COLOUR_SERVER_TARGET_PROFILE_IN_X_BASE"%s%s", 
            screen ? "_" : "", screen ? num : "" );
  else
  snprintf( icc_profile_atom, 1024, OY_ICC_V0_3_TARGET_PROFILE_IN_X_BASE"%s%s", 
            screen ? "_" : "", screen ? num : "" );


  a = XInternAtom(s->display->display, icc_profile_atom, False);

  data = fetchProperty( s->display->display, root, a, XA_CARDINAL,
                          &n, False);
  if(data) XFree(data);
  free(icc_profile_atom);
  return (int)n;
}

static int     cleanScreenProfile    ( CompScreen        * s,
                                       int                 screen,
                                       int                 server )
{
  char num[12];
  Window root = RootWindow( s->display->display, 0 );
  char * icc_profile_atom = (char*)calloc( 1024, sizeof(char) );
  Atom a;

  if(!icc_profile_atom) return 0;

  snprintf( num, 12, "%d", (int)screen );
  if(server)
  snprintf( icc_profile_atom, 1024, OY_ICC_COLOUR_SERVER_TARGET_PROFILE_IN_X_BASE"%s%s", 
            screen ? "_" : "", screen ? num : "" );
  else
  snprintf( icc_profile_atom, 1024, OY_ICC_V0_3_TARGET_PROFILE_IN_X_BASE"%s%s", 
            screen ? "_" : "", screen ? num : "" );


  a = XInternAtom(s->display->display, icc_profile_atom, False);

  XFlush( s->display->display );
  XDeleteProperty( s->display->display, root, a );
  return (int)0;
}

static void changeProperty           ( Display           * display,
                                       Atom                target_atom,
                                       int                 type,
                                       void              * data,
                                       unsigned long       size )
{
    XChangeProperty( display, RootWindow( display, 0 ),
                     target_atom, type, 8, PropModeReplace,
                     data, size );
}

static void    moveICCprofileAtoms   ( CompScreen        * s,
                                       int                 screen,
                                       int                 init )
{
  PrivScreen * ps = compObjectGetPrivate((CompObject *) s);
  char num[12];
  Window root = RootWindow( s->display->display, 0 );
  char * icc_profile_atom = (char*)calloc( 1024, sizeof(char) ),
       * icc_colour_server_profile_atom = (char*)calloc( 1024, sizeof(char) );
  Atom a,da, source_atom, target_atom;

  oyPointer source;
  oyPointer target;
  unsigned long source_n = 0, target_n = 0;
  int updated_icc_color_desktop_atom = 0;

  snprintf( num, 12, "%d", (int)screen );
  snprintf( icc_profile_atom, 1024, OY_ICC_V0_3_TARGET_PROFILE_IN_X_BASE"%s%s", 
            screen ? "_" : "", screen ? num : "" );
  snprintf( icc_colour_server_profile_atom, 1024, OY_ICC_COLOUR_SERVER_TARGET_PROFILE_IN_X_BASE"%s%s", 
            screen ? "_" : "", screen ? num : "" );

  a = XInternAtom(s->display->display, icc_profile_atom, False);
  da = XInternAtom(s->display->display, icc_colour_server_profile_atom, False);

  /* select the atoms */
  if(init)
  {
    source_atom = a;
    target_atom = da;
  } else
  {
    source_atom = da;
    target_atom = a;
  }

  target = fetchProperty( s->display->display, root, target_atom, XA_CARDINAL,
                          &target_n, False);

  if( !target_n ||
      (target_n && !init) )
  {
    /* copy the real device atom */
    source = fetchProperty( s->display->display, root, source_atom, XA_CARDINAL,
                            &source_n, False);

    /* _ICC_COLOR_DESKTOP atom is set before any _ICC_PROFILE(_xxx) changes. */
    if(init)
    {
      updateNetColorDesktopAtom( s, ps, 2 );
      updated_icc_color_desktop_atom = 1;
    }
    if(source_n)
    {
      changeProperty ( s->display->display,
                       target_atom, XA_CARDINAL,
                       source, source_n );
    }
    XFree( source );
    source = 0; source_n = 0;

    if(init)
    {
      /* setup the OY_ICC_V0_3_TARGET_PROFILE_IN_X_BASE(_xxx) atom as document colour space */
      size_t size = 0;
      oyProfile_s * screen_document_profile = oyProfile_FromStd( oyASSUMED_WEB,
                                                                 0 );

      if(!screen_document_profile)
        oyCompLogMessage( s->display, "compicc", CompLogLevelWarn,
                          DBG_STRING"Could not get oyASSUMED_WEB", DBG_ARGS);

      /* make shure the profile is ignored */

      source = oyProfile_GetMem( screen_document_profile, &size, 0, malloc );
      source_n = size;

      if(!updated_icc_color_desktop_atom)
      {
        updateNetColorDesktopAtom( s, ps, 2 );
        updated_icc_color_desktop_atom = 1;
      }
      if(source_n)
      {
        changeProperty ( s->display->display,
                         source_atom, XA_CARDINAL,
                         source, source_n );
      }
      oyProfile_Release( &screen_document_profile );
      if(source) free( source ); source = 0;
    } else
    {
      /* clear/erase the _ICC_DEVICE_PROFILE(_xxx) atom */
      XDeleteProperty( s->display->display,root, source_atom );
    }

  } else
    if(target_atom && init)
      oyCompLogMessage( s->display, "compicc", CompLogLevelWarn,
                        DBG_STRING"icc_colour_server_profile_atom already present %d size:%lu",
                        DBG_ARGS, target_atom, target_n );

  if(icc_profile_atom) free(icc_profile_atom);
  if(icc_colour_server_profile_atom) free(icc_colour_server_profile_atom);
}

void           cleanDisplay          ( Display           * display )
{
  int error = 0,
      n;
  oyOptions_s * options = 0;
  oyConfigs_s * devices = 0;
  char * display_name = 0, * t;
  int old_oy_debug, i;

    display_name = strdup(XDisplayString(display));
    if(display_name && strchr(display_name,'.'))
    {
      t = strrchr(display_name,'.');
      t[0] = 0;
    }

    /* clean up old displays */
    error = oyOptions_SetFromText( &options,
                                   "//"OY_TYPE_STD"/config/command",
                                   "unset", OY_CREATE_NEW );
    if(display_name)
    {
      t = calloc(sizeof(char), strlen(display_name) + 8);
    } else
    {
      display_name = strdup(":0");
      t = calloc(sizeof(char), 8);
    }

    if(t && display_name)
    {
      for(i = 0; i < 200; ++i)
      {
        sprintf( t, "%s.%d", display_name, i );
        error = oyOptions_SetFromText( &options,
                                       "//" OY_TYPE_STD "/config/device_name",
                                       t, OY_CREATE_NEW );
        error = oyDevicesGet( OY_TYPE_STD, "monitor", options, &devices );
        if(error != 0) i = 200;
        oyConfigs_Release( &devices );
      }
    }
    oyOptions_Release( &options );


    /* get number of connected devices */
    error = oyOptions_SetFromText( &options,
                                   "//"OY_TYPE_STD"/config/command",
                                   "list", OY_CREATE_NEW );
    error = oyOptions_SetFromText( &options,
                                   "//" OY_TYPE_STD "/config/display_name",
                                   display_name, OY_CREATE_NEW );
    error = oyDevicesGet( OY_TYPE_STD, "monitor", options, &devices );
    n = oyConfigs_Count( devices );
    oyConfigs_Release( &devices );
    oyOptions_Release( &options );

    /** Monitor hotplugs can easily mess up the ICC profile to device assigment.
     *  So first we erase the _ICC_PROFILE(_xxx) to get a clean state.
     *  We setup the EDID atoms and ICC profiles new.
     *  The ICC profiles are moved to the right places through the 
     *  PropertyChange events recieved by the colour server.
     */

    /* refresh EDID */
    error = oyOptions_SetFromText( &options, "//" OY_TYPE_STD "/config/command",
                                   "list", OY_CREATE_NEW );
    sprintf( t, "%s.%d", display_name, 0 );
    error = oyOptions_SetFromText( &options,
                                   "//" OY_TYPE_STD "/config/device_name",
                                   t, OY_CREATE_NEW );
    error = oyOptions_SetFromText( &options, "//" OY_TYPE_STD "/config/edid",
                                   "refresh", OY_CREATE_NEW );
    old_oy_debug = oy_debug;
    /*oy_debug = 1;*/
    error = oyDevicesGet( OY_TYPE_STD, "monitor", options, &devices );
    oy_debug = old_oy_debug;
    oyConfigs_Release( &devices );
    oyOptions_Release( &options );

    free(display_name); display_name = 0;
    free(t); t = 0;
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
    r = (oyRectangle_s*) oyOption_StructGet( o, oyOBJECT_RECTANGLE_S );
    if( !r )
    {
      oyCompLogMessage( s->display, "compicc", CompLogLevelWarn,
                      DBG_STRING"monitor rectangle request failed", DBG_ARGS);
      return 1;
    }
    oyOption_Release( &o );

    output->xRect.x = r->x;
    output->xRect.y = r->y;
    output->xRect.width = r->width;
    output->xRect.height = r->height;

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

    {
      oyOptions_s * options = 0;
      oyOptions_SetFromText( &options,
                   "//"OY_TYPE_STD"/config/command",
                                       "list", OY_CREATE_NEW );
      oyOptions_SetFromText( &options,
                   "//"OY_TYPE_STD"/config/icc_profile.x_color_region_target",
                                       "yes", OY_CREATE_NEW );
      t_err = oyDeviceGetProfile( device, options, &output->cc.dst_profile );
      oyOptions_Release( &options );
    }

    if(output->cc.dst_profile)
    {
      /* check that no sRGB is delivered */
      if(t_err)
      {
        oyProfile_s * web = oyProfile_FromStd( oyASSUMED_WEB, 0 );
        if(oyProfile_Equal( web, output->cc.dst_profile ))
        {
          oyCompLogMessage( s->display, "compicc", CompLogLevelWarn,
                      DBG_STRING "Output %s ignoring fallback %d",
                      DBG_ARGS, output->name, error);
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

static void    setupColourTable      ( PrivColorContext  * ccontext,
                                       int                 advanced )
{
  oyConversion_s * cc;
  int error = 0;

    if (ccontext->dst_profile)
    {
      int flags = 0;

      oyProfile_s * src_profile = ccontext->src_profile,
                  * dst_profile = ccontext->dst_profile;
      oyOptions_s * options = 0;

      oyPixel_t pixel_layout = OY_TYPE_123_16;

      if(!src_profile)
        src_profile = oyProfile_FromStd( oyASSUMED_WEB, 0 );

      if(!src_profile)
        oyCompLogMessage(NULL, "compicc", CompLogLevelWarn,
             DBG_STRING "Output %s: no oyASSUMED_WEB src_profile",
             DBG_ARGS, ccontext->output_name );

      /* optionally set advanced options from Oyranos */
      if(advanced)
        flags = oyOPTIONATTRIBUTE_ADVANCED;

      if(oy_debug)
        oyCompLogMessage( NULL, "compicc", CompLogLevelWarn,
                      DBG_STRING "oyConversion_Correct(///icc,%d,0) %s %s",
                      DBG_ARGS, flags, ccontext->output_name,
                      advanced?"advanced":"");
      oyImage_s * image_in = oyImage_Create( GRIDPOINTS,GRIDPOINTS*GRIDPOINTS,
                                             ccontext->clut,
                                             pixel_layout, src_profile, 0 );
      oyImage_s * image_out= oyImage_Create( GRIDPOINTS,GRIDPOINTS*GRIDPOINTS,
                                             ccontext->clut,
                                             pixel_layout, dst_profile, 0 );

      oyProfile_Release( &src_profile );

      cc = oyConversion_CreateBasicPixels( image_in, image_out,
                                                      options, 0 );
      if (cc == NULL)
      {
        oyCompLogMessage( NULL, "compicc", CompLogLevelWarn,
                      DBG_STRING "no conversion created for %s",
                      DBG_ARGS, ccontext->output_name);
        return;
      }
      oyOptions_Release( &options );

      error = oyOptions_SetFromText( &options,
                                     "//"OY_TYPE_STD"/config/display_mode", "1",
                                     OY_CREATE_NEW );
      error = oyConversion_Correct(cc, "//" OY_TYPE_STD "/icc", flags, options);
      if(error)
      {
        oyCompLogMessage( NULL, "compicc", CompLogLevelWarn,
                      DBG_STRING "oyConversion_Correct(///icc,%d,0) failed %s",
                      DBG_ARGS, flags, ccontext->output_name);
        return;
      }

      oyFilterGraph_s * cc_graph = oyConversion_GetGraph( cc );
      oyFilterNode_s * icc = oyFilterGraph_GetNode( cc_graph, -1, "///icc", 0 );

      uint32_t exact_hash_size = 0;
      char * hash_text = 0;
      const char * t = 0;
      {
        t = oyFilterNode_GetText( icc, oyNAME_NICK );
        if(t)
          hash_text = strdup(t);
      }
      oyHash_s * entry;
      oyArray2d_s * clut = NULL;
      oyStructList_s * cache = pluginGetPrivatesCache();
      entry = oyCacheListGetEntry_( cache, exact_hash_size, hash_text );
      clut = (oyArray2d_s*) oyHash_GetPointer( entry, oyOBJECT_ARRAY2D_S);
      oyFilterNode_Release( &icc );
      oyFilterGraph_Release( &cc_graph );

      if(clut)
        memcpy( ccontext->clut, clut->array2d[0], 
                sizeof(GLushort) * GRIDPOINTS*GRIDPOINTS*GRIDPOINTS * 3 );
      else
      {
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
          return;
        }

        memcpy( clut->array2d[0], ccontext->clut,
                sizeof(GLushort) * GRIDPOINTS*GRIDPOINTS*GRIDPOINTS * 3 );

        oyHash_SetPointer( entry, (oyStruct_s*) clut );
      }

      if(hash_text)
      {
        free(hash_text); hash_text = 0;
      }


      oyOptions_Release( &options );
      oyImage_Release( &image_in );
      oyImage_Release( &image_out );
      oyConversion_Release( &cc );

      cdCreateTexture( ccontext );

    } else {
      oyCompLogMessage( NULL, "compicc", CompLogLevelInfo,
                      DBG_STRING "Output \"%s\": no profile",
                      DBG_ARGS, ccontext->output_name);
    }

}

static int     getDisplayAdvanced    ( CompScreen        * s,
                                       int                 screen )
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
        XFree( opt ); opt = 0;

  return advanced;
}

static void    setupOutputTable      ( CompScreen        * s,
                                       oyConfig_s        * device,
                                       int                 screen )
{
  PrivScreen *ps = compObjectGetPrivate((CompObject *) s);
  PrivColorOutput * output = &ps->contexts[screen];


  if(!colour_desktop_can)
    return;


  output->cc.src_profile = oyProfile_FromStd( oyASSUMED_WEB, 0 );
  output->cc.output_name = strdup(output->name);
  if(!output->cc.src_profile)
    oyCompLogMessage(s->display, "compicc", CompLogLevelWarn,
             DBG_STRING "Output %s: no oyASSUMED_WEB src_profile",
             DBG_ARGS, output->name );

  setupColourTable( &output->cc, getDisplayAdvanced( s, screen ) );
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
    free(ps->contexts);
  }
}


void cleanDisplayProfiles( CompScreen *s )
{
  int error = 0,
      n,
      screen;
  oyConfigs_s * devices = 0;

    /* get number of connected devices */
    error = oyDevicesGet( OY_TYPE_STD, "monitor", 0, &devices );
    n = oyConfigs_Count( devices );
    oyConfigs_Release( &devices );

    for(screen = 0; screen < n; ++screen)
    {
      int server_profile = 1;
      if(hasScreenProfile( s, screen, server_profile ))
        cleanScreenProfile( s, screen, server_profile );
    }
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
    ps->contexts = (PrivColorOutput*)calloc( ps->nContexts,
                                             sizeof(PrivColorOutput ));
    for(i = 0; i < n; ++i)
      ps->contexts[i].cc.ref = 1;
  }

  /* allow Oyranos to see modifications made to the compiz Xlib context */
  XFlush( s->display->display );

  cleanDisplay( s->display->display );
}

/**
 * Called when XRandR output configuration (or properties) change. Fetch
 * output profiles (if available) or fall back to sRGB.
 * Device profiles are obtained from Oyranos only once at beginning.
 */
static void updateOutputConfiguration(CompScreen *s, CompBool init)
{
  PrivScreen *ps = compObjectGetPrivate((CompObject *) s);
  int error = 0,
      n,
      set = 1;
  oyOptions_s * options = 0;
  oyConfigs_s * devices = 0;
  oyConfig_s * device = 0;

  /* allow Oyranos to see modifications made to the compiz Xlib context */
  XFlush( s->display->display );

  /* obtain device informations, including geometry and ICC profiles
     from the according Oyranos module */
  error = oyOptions_SetFromText( &options, "//" OY_TYPE_STD "/config/command",
                                 "list", OY_CREATE_NEW );
  error = oyOptions_SetFromText( &options, "//" OY_TYPE_STD "/config/device_rectangle",
                                 "true", OY_CREATE_NEW );
  error = oyDevicesGet( OY_TYPE_STD, "monitor", options, &devices );
  n = oyOptions_Count( options );
  oyOptions_Release( &options );

  n = oyConfigs_Count( devices );

  if(colour_desktop_can)
  for (unsigned long i = 0; i < ps->nContexts; ++i)
  {
    device = oyConfigs_Get( devices, i );

    if(init)
      error = getDeviceProfile( s, ps, device, i );

    if(ps->contexts[i].cc.dst_profile)
    {
      moveICCprofileAtoms( s, i, set );
      setupOutputTable( s, device, i );
    } else
    {
      oyCompLogMessage( s->display, "compicc", CompLogLevelDebug,
                  DBG_STRING "No profile found on desktops %d/%d 0x%lx 0x%lx",
                  DBG_ARGS, i, ps->nContexts, &ps->contexts[i], ps->contexts[i].cc.dst_profile);
    }

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
      CompScreen *s = findScreenAtDisplay(d, event->xproperty.window);
      CompWindow *w = findWindowAtDisplay(d, event->xproperty.window);
      updateWindowRegions(w);
      colour_desktop_region_count = -1;
    } else if (event->xproperty.atom == pd->iccColorTarget)
    {
      CompWindow *w = findWindowAtDisplay(d, event->xproperty.window);
      updateWindowOutput(w);

    /* let possibly others take over the colour server */
    } else if( event->xproperty.atom == pd->iccColorDesktop && atom_name )
    {
      updateNetColorDesktopAtom( s, ps, 0 );

    /* update for a changing monitor profile */
    } else if(
           strstr( atom_name, OY_ICC_V0_3_TARGET_PROFILE_IN_X_BASE) != 0/* &&
           strstr( atom_name, "ICC_PROFILE_IN_X") == 0*/ )
    {
      if(colour_desktop_can)
      {
        int screen = 0;
        int ignore_profile = 0;
        char * icc_colour_server_profile_atom = (char*)malloc(1024);
        char num[12];
        Atom da;
        unsigned long n = 0;

        if(strlen(atom_name) > strlen(OY_ICC_V0_3_TARGET_PROFILE_IN_X_BASE"_"))
        sscanf( (const char*)atom_name,
                OY_ICC_V0_3_TARGET_PROFILE_IN_X_BASE "_%d", &screen );
        snprintf( num, 12, "%d", (int)screen );

        snprintf( icc_colour_server_profile_atom, 1024,
                  OY_ICC_COLOUR_SERVER_TARGET_PROFILE_IN_X_BASE"%s%s",
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
            oyProfile_s * web = oyProfile_FromStd( oyASSUMED_WEB, 0 );

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
              if(ps->nContexts > screen)
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

        if(icc_colour_server_profile_atom) free(icc_colour_server_profile_atom);

        if(!ignore_profile &&
           /* change only existing profiles, ignore removed ones */
           n)
        {
          updateOutputConfiguration( s, FALSE );
        }
      }

    /* update for changing geometry */
    } else if (event->xproperty.atom == pd->netDesktopGeometry)
    {
      setupOutputs( s );
      updateOutputConfiguration(s, TRUE);
    } else if (event->xproperty.atom == pd->iccDisplayAdvanced)
    {
      updateOutputConfiguration( s, FALSE );
    }

    break;
  case ClientMessage:
    if (event->xclient.message_type == pd->iccColorManagement)
    {
      CompWindow *w = findWindowAtDisplay (d, event->xclient.window);
      PrivWindow *pw = compObjectGetPrivate((CompObject *) w);

      pw->active = 1;
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
        {
          setupOutputs( s );
          updateOutputConfiguration(s, TRUE);
        }
      }
    }
#endif
    break;
  }

  /* initialise */
  if(s && ps && s->nOutputDev != ps->nContexts)
  {
    setupOutputs( s );
    updateOutputConfiguration( s, TRUE);
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
    if(pw->nRegions > 1)
    {
      pw->stencil_id_start = *count;
      *count = *count + pw->nRegions - 1;
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
  int i,j;

  /* check every 10 seconds */
  time_t  cutime;         /* Time since epoch */
  cutime = time(NULL);    /* current user time */
  if((cutime - icc_color_desktop_last_time > (time_t)10))
    updateNetColorDesktopAtom( s, ps, 0 );

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

  oyRectangle_s * rect = oyRectangle_NewWith( w->serverX, w->serverY, w->serverWidth, w->serverHeight, 0 );

  /* update to window movements and resizes */
  if( !oyRectangle_IsEqual( rect, pw->absoluteWindowRectangleOld ) )
  {
    forEachWindowOnScreen(s, damageWindow, NULL);

    if(w->serverWidth != pw->absoluteWindowRectangleOld->width ||
       w->serverHeight != pw->absoluteWindowRectangleOld->height)
      updateWindowRegions( w );

    oyRectangle_SetByRectangle( pw->absoluteWindowRectangleOld, rect );

  }

  oyRectangle_Release( &rect );

  /* skip the stencil drawing for to be scissored windows */
  if( !HAS_REGIONS(pw) )
    return status;

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

  if( HAS_REGIONS(pw) )
  {
    glEnable(GL_STENCIL_TEST);
    glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
  } else
    glEnable(GL_SCISSOR_TEST);

  int i,j;
  for(i = 0; i < ps->nContexts; ++i)
  {
    Region tmp = 0;
    Region screen = 0;
    Region intersection = 0;
    /* draw the texture over the whole monitor to affect wobbly windows */
    XRectangle * r = &ps->contexts[i].xRect;
    glScissor( r->x, s->height - r->y - r->height, r->width, r->height);

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

      PrivColorContext * c = window_region->cc;
      // TODO
      if(j == pw->nRegions - 1)
      {
        c = &ps->contexts[i].cc;
        if(!c)
          oyCompLogMessage( s->display, "compicc", CompLogLevelWarn,
                    DBG_STRING "No CLUT found for screen %d / %d / %d",
                    DBG_ARGS, screen, ps->nContexts, j );
      }

      BOX * b = &intersection->extents;

      if(!c || (b->x1 == 0 && b->x2 == 0 && b->y1 == 0 && b->y2 == 0))
        goto cleanDrawTexture;

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

      /* Only draw where the stencil value matches the window and output */
      glStencilFunc(GL_EQUAL, STENCIL_ID, ~0);


      /* Now draw the window texture */
      UNWRAP(ps, s, drawWindowTexture);
      if(c->dst_profile && c->glTexture)
        (*s->drawWindowTexture) (w, texture, &fa, mask);
      else
        /* ignore the shader */
        (*s->drawWindowTexture) (w, texture, attrib, mask);
      WRAP(ps, s, drawWindowTexture, pluginDrawWindowTexture);

      cleanDrawTexture:
      if(intersection)
        XDestroyRegion( intersection );
      if(tmp)
        XDestroyRegion( tmp );
      if(screen)
        XDestroyRegion( screen );
    }
  }

  /* Deactivate the 3D texture */
  (*s->activeTexture) (GL_TEXTURE0_ARB + unit);
  glBindTexture(GL_TEXTURE_3D, 0);
  glDisable(GL_TEXTURE_3D);
  (*s->activeTexture) (GL_TEXTURE0_ARB);

  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
}


/**
 *    Object Init Functions
 */

static CompBool pluginInitCore(CompPlugin *plugin, CompObject *object, void *privateData)
{
#if defined(PLUGIN_DEBUG_)
  int dbg_switch = 60;

  while(dbg_switch--)
    sleep(1);
#endif

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
 *    - ICP  _ICC_COLOR_PROFILES
 *    - ICT  _ICC_COLOR_TARGET
 *    - ICM  _ICC_COLOR_MANAGEMENT
 *    - ICR  _ICC_COLOR_REGIONS
 *    - ICA  _ICC_COLOR_DISPLAY_ADVANCED
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
static int updateNetColorDesktopAtom ( CompScreen        * s,
                                       PrivScreen        * ps,
                                       int                 request )
{
  CompDisplay * d = s->display;
  PrivDisplay * pd = compObjectGetPrivate((CompObject *) d);
  time_t  cutime;         /* Time since epoch */
  cutime = time(NULL);    /* current user time */
  const char * my_id = "compicc",
             * my_capabilities = "|ICP|ICR|ICA|V0.3|"; /* _ICC_COLOR_REGIONS
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

  atom_colour_server_name = (char*)malloc(1024);
  atom_capabilities_text = (char*)malloc(1024);
  if(!atom_colour_server_name || !atom_capabilities_text)
  {
    status = 3;
    goto clean_updateNetColorDesktopAtom;
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
      if(atom_time < icc_color_desktop_last_time ||
         request == 2)
      {
        oyCompLogMessage( d, "compicc", CompLogLevelWarn,
                    DBG_STRING "\nTaking over colour service from old _ICC_COLOR_DESKTOP: %s.",
                    DBG_ARGS, old_atom ? old_atom : "????" );
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
  for(int i = 0; i < ps->nContexts; ++i)
    attached_profiles += ps->contexts[i].cc.dst_profile ? 1:0;

  int transform_n = 0;
  for(int i = 0; i < ps->nContexts; ++i)
    transform_n += ps->contexts[i].cc.glTexture ? 1:0;

  if( (atom_time + 10) < icc_color_desktop_last_time ||
      request == 2 )
  {
    char * atom_text = (char*)malloc(1024);
    if(!atom_text) goto clean_updateNetColorDesktopAtom;
    sprintf( atom_text, "%d %ld %s %s",
             (int)pid, (long)cutime,
             /* say if we can convert, otherwise give only the version number */
             transform_n ? my_capabilities : "|V0.3|",
             my_id );
 
   if(attached_profiles)
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

    if(atom_text) free( atom_text );
  }

clean_updateNetColorDesktopAtom:
  if(atom_colour_server_name) free(atom_colour_server_name);
  if(atom_capabilities_text) free(atom_capabilities_text);

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

static CompBool pluginInitDisplay(CompPlugin *plugin, CompObject *object, void *privateData)
{
  CompDisplay *d = (CompDisplay *) object;
  PrivDisplay *pd = privateData;

  if (d->randrExtension == False)
    return FALSE;

  WRAP(pd, d, handleEvent, pluginHandleEvent);

  printf( DBG_STRING "HUHU\n", DBG_ARGS );

  pd->iccColorManagement = XInternAtom(d->display, "_ICC_COLOR_MANAGEMENT", False);

  pd->iccColorProfiles = XInternAtom(d->display, XCM_COLOR_PROFILES, False);
  pd->iccColorRegions = XInternAtom(d->display, XCM_COLOR_REGIONS, False);
  pd->iccColorTarget = XInternAtom(d->display, XCM_COLOR_TARGET, False);
  pd->iccColorDesktop = XInternAtom(d->display, XCM_COLOR_DESKTOP, False);
  pd->netDesktopGeometry = XInternAtom(d->display, "_NET_DESKTOP_GEOMETRY", False);
  pd->iccDisplayAdvanced = XInternAtom(d->display, XCM_COLOUR_DESKTOP_ADVANCED, False);

  return TRUE;
}


static CompBool pluginInitScreen(CompPlugin *plugin, CompObject *object, void *privateData)
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
    return FALSE;

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
                  RROutputPropertyNotifyMask);
#endif

  /* initialisation is done in pluginHandleEvent() by checking ps->nContexts */
  ps->nContexts = 0;

  return TRUE;
}

static CompBool pluginInitWindow(CompPlugin *plugin, CompObject *object, void *privateData)
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


static CompBool pluginFiniCore(CompPlugin *plugin, CompObject *object, void *privateData)
{
  return TRUE;
}

static CompBool pluginFiniDisplay(CompPlugin *plugin, CompObject *object, void *privateData)
{
  CompDisplay *d = (CompDisplay *) object;
  PrivDisplay *pd = privateData;

  UNWRAP(pd, d, handleEvent);

  /* remove desktop colour management service mark */
  changeProperty( d->display,
                                pd->iccColorDesktop, XA_STRING,
                                (unsigned char*)NULL, 0 );

  return TRUE;
}

static CompBool pluginFiniScreen(CompPlugin *plugin, CompObject *object, void *privateData)
{
  CompScreen *s = (CompScreen *) object;
  PrivScreen *ps = privateData;

  int error = 0,
      n,
      init = 0;
  oyConfigs_s * devices = 0;
  oyConfig_s * device = 0;

  error = oyDevicesGet( OY_TYPE_STD, "monitor", 0, &devices );

  n = oyConfigs_Count( devices );

  /* switch profile atoms back */
  for(int i = 0; i < ps->nContexts; ++i)
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

static CompBool pluginFiniWindow(CompPlugin *plugin, CompObject *object, void *privateData)
{
  return TRUE;
}

static dispatchObjectProc dispatchFiniObject[] = {
  pluginFiniCore, pluginFiniDisplay, pluginFiniScreen, pluginFiniWindow
};


/**
 *    Plugin Interface
 */
static CompBool pluginInit(CompPlugin *p)
{
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
    o->privates[index].ptr = malloc(size);
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

static void pluginFini(CompPlugin *p)
{
  oyStructList_Release( &privates_cache );
}

static CompMetadata *pluginGetMetadata(CompPlugin *p)
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

