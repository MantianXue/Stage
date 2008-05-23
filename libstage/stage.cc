#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <assert.h>
#include <unistd.h>
#include <math.h>
#include <glib.h>
#include <locale.h>

#include <FL/Fl_Shared_Image.H>

//#define DEBUG

#include "stage_internal.hh"
//#include "config.h" // results of autoconf's system configuration tests

static bool init_called = false;
static GHashTable* typetable = NULL;


void Stg::Init( int* argc, char** argv[] )
{
  PRINT_DEBUG( "Stg::Init()" );

  // seed the RNG 
  srand48( time(NULL) );
  
  if(!setlocale(LC_ALL,"POSIX"))
    PRINT_WARN("Failed to setlocale(); config file may not be parse correctly\n" );
  
  typetable = stg_create_typetable();      
  init_called = true;  

  // ask FLTK to load support for various image formats
  fl_register_images();
}

bool Stg::InitDone()
{
  return init_called;
}

GHashTable* Stg::Typetable()
{
  return typetable;
}


void Stg::stg_print_err( const char* err )
{
  printf( "Stage error: %s\n", err );
  //_stg_quit = TRUE;
}


/** Visit every voxel along a vector from (x,y,z) to (x+dx, y+dy, z+dz).
    Call the function for every voxel, passing in the current voxel
    coordinates followed by the two arguments. Adapted from Graphics
    Gems IV, algorithm by Cohen & Kaufman 1991 */
int Stg::stg_line_3d( int32_t x, int32_t y, int32_t z,
		 int32_t dx, int32_t dy, int32_t dz,
		 stg_line3d_func_t visit_voxel,
		 void* arg )
{
  int n, sx, sy, sz, exy, exz, ezy, ax, ay, az, bx, by, bz;
  
  sx = sgn(dx);  sy = sgn(dy);  sz = sgn(dz);
  ax = abs(dx);  ay = abs(dy);  az = abs(dz);
  bx = 2*ax;	   by = 2*ay;	  bz = 2*az;
  exy = ay-ax;   exz = az-ax;	  ezy = ay-az;
  n = ax+ay+az;
  while ( n-- ) {
    if((*visit_voxel)( x, y, z, arg ) )
      {
	return TRUE; // hit something!
      }

    if ( exy < 0 ) {
      if ( exz < 0 ) {
	x += sx;
	exy += by; exz += bz;
      }
      else  {
	z += sz;
	exz -= bx; ezy += by;
      }
    }
    else {
      if ( ezy < 0 ) {
	z += sz;
	exz -= bx; ezy += by;
      }
      else  {
	y += sy;
	exy -= bx; ezy -= bz;
      }
    }
  }

  return FALSE; // hit nothing (leave vars not set)
}

int Stg::stg_polygon_3d( stg_point_int_t* pts, unsigned int pt_count,
		    stg_line3d_func_t visit_voxel,
		    void* arg )
{
  assert( pts );
  
  for( unsigned int p=0; p<pt_count; p++ )
    {
      int32_t x = pts[p].x; 
      int32_t y = pts[p].y; 
      
      int32_t dx = pts[(p+1)%pt_count].x - x;
      int32_t dy = pts[(p+1)%pt_count].y - y;
      
      if( stg_line_3d( x, y, 0,
		       dx, dy, 0,
		       visit_voxel, arg ) )
	return TRUE;
    }
  return FALSE;
}

void Stg::stg_print_geom( stg_geom_t* geom )
{
  printf( "geom pose: (%.2f,%.2f,%.2f) size: [%.2f,%.2f]\n",
	  geom->pose.x,
	  geom->pose.y,
	  geom->pose.a,
	  geom->size.x,
	  geom->size.y );
}


void Stg::stg_print_pose( stg_pose_t* pose )
{
  printf( "pose [x:%.3f y:%.3f a:%.3f]\n",
	  pose->x, pose->y, pose->a );
}

void Stg::stg_print_velocity( stg_velocity_t* vel )
{
  printf( "velocity [x:%.3f y:%.3f a:%.3f]\n",
	  vel->x, vel->y, vel->a );
}

stg_msec_t Stg::stg_realtime( void )
{
  struct timeval tv;
  gettimeofday( &tv, NULL );  
  stg_msec_t timenow = (stg_msec_t)( tv.tv_sec*1e3 + tv.tv_usec/1e3 );
  return timenow;
}

stg_msec_t Stg::stg_realtime_since_start( void )
{
  static stg_msec_t starttime = 0;  
  stg_msec_t timenow = stg_realtime();
  
  if( starttime == 0 )
    starttime = timenow;
    
  return( timenow - starttime );
}


// Look up the color in a database.  (i.e. transform color name to
// color value).  If the color is not found in the database, a bright
// red color will be returned instead.
stg_color_t Stg::stg_lookup_color(const char *name)
{
  if( name == NULL ) // no string?
    return 0; // black
  
  if( strcmp( name, "" ) == 0 ) // empty string?
    return 0; // black
  
  static FILE *file = NULL;
  static GHashTable* table = g_hash_table_new( g_str_hash, g_str_equal );
  
  if( file == NULL )
    {
      char* searchfiles[] = {
	"./rgb.txt",
#ifdef RGBFILE
	RGBFILE,
#endif
	"../rgb.txt",
	NULL };
      
      for( int i=0;
	   searchfiles[i];
	   i++ )
	{
	  char* filename = searchfiles[i];
	  PRINT_DEBUG1( "Attempting to open \"%s\"", filename );
	  if( (file = fopen( filename, "r")) )
	    break; // opened a file ok - jump out of for loop
	}
      
      if( file == NULL )
	{
	  
	  PRINT_ERR1("unable to open color database: %s",
		     strerror(errno));
	  fclose(file);
	  exit(0);
	}
      
      PRINT_DEBUG( "Success!" );

      // load the file into the hash table       
      while (TRUE)
	{
	  char line[1024];
	  if (!fgets(line, sizeof(line), file))
	    break;
	  
	  // it's a macro or comment line - ignore the line
	  if (line[0] == '!' || line[0] == '#' || line[0] == '%') 
	    continue;

	  // Trim the trailing space
	  while (strchr(" \t\n", line[strlen(line)-1]))
	    line[strlen(line)-1] = 0;
	  
	  // Read the color
	  int r, g, b;
	  int chars_matched = 0;
	  sscanf( line, "%d %d %d %n", &r, &g, &b, &chars_matched );
	  
	  
	  stg_color_t* col = new stg_color_t;
	  *col = ( 0xFF000000 | (r << 16) | (g << 8) | b);
	  
	  // Read the name
	  char* colorname = strdup( line + chars_matched );
	  
	  // map the name to the color in the table
	  g_hash_table_insert( table, (gpointer)colorname, (gpointer)col );
	  
	}

      fclose(file);
    }

  // look up the colorname in the database  
  stg_color_t* found = (stg_color_t*)g_hash_table_lookup( table, name );

  if( found )
    return *found;
  else
    return (stg_color_t)0;
}

//////////////////////////////////////////////////////////////////////////
// scale an array of rectangles so they fit in a unit square
void Stg::stg_rotrects_normalize( stg_rotrect_t* rects, int num )
{
  // assuming the rectangles fit in a square +/- one billion units
  double minx, miny, maxx, maxy;
  minx = miny = billion;
  maxx = maxy = -billion;
  
  int r;
  for( r=0; r<num; r++ )
    {
      // test the origin of the rect
      if( rects[r].pose.x < minx ) minx = rects[r].pose.x;
      if( rects[r].pose.y < miny ) miny = rects[r].pose.y;      
      if( rects[r].pose.x > maxx ) maxx = rects[r].pose.x;      
      if( rects[r].pose.y > maxy ) maxy = rects[r].pose.y;

      // test the extremes of the rect
      if( (rects[r].pose.x+rects[r].size.x)  < minx ) 
	minx = (rects[r].pose.x+rects[r].size.x);
      
      if( (rects[r].pose.y+rects[r].size.y)  < miny ) 
	miny = (rects[r].pose.y+rects[r].size.y);
      
      if( (rects[r].pose.x+rects[r].size.x)  > maxx ) 
	maxx = (rects[r].pose.x+rects[r].size.x);
      
      if( (rects[r].pose.y+rects[r].size.y)  > maxy ) 
	maxy = (rects[r].pose.y+rects[r].size.y);
    }
  
  // now normalize all lengths so that the rects all fit inside
  // rectangle from 0,0 to 1,1
  double scalex = maxx - minx;
  double scaley = maxy - miny;

  for( r=0; r<num; r++ )
    { 
      rects[r].pose.x = (rects[r].pose.x - minx) / scalex;
      rects[r].pose.y = (rects[r].pose.y - miny) / scaley;
      rects[r].size.x = rects[r].size.x / scalex;
      rects[r].size.y = rects[r].size.y / scaley;
    }
}	

// sets [result] to the pose of [p2] in [p1]'s coordinate system
void Stg::stg_pose_sum( stg_pose_t* result, stg_pose_t* p1, stg_pose_t* p2 )
{
  double cosa = cos(p1->a);
  double sina = sin(p1->a);
  
  result->x = p1->x + p2->x * cosa - p2->y * sina;
  result->y = p1->y + p2->x * sina + p2->y * cosa;
  result->z = p1->z + p2->z;
  result->a = normalize(p1->a + p2->a);

  //  printf( "pose z %.2f\n", result->z );
}

// returns the resultant of vector [p1] and [p2] 
stg_pose_t Stg::pose_sum( stg_pose_t p1, stg_pose_t p2 )
{
  double cosa = cos(p1.a);
  double sina = sin(p1.a);
  
  stg_pose_t result;  
  result.x = p1.x + p2.x * cosa - p2.y * sina;
  result.y = p1.y + p2.x * sina + p2.y * cosa;
  result.z = p1.z + p2.z;
  result.a = normalize(p1.a + p2.a);

  return result;
}

// pb_* functions are only used inside this file

// static guchar* pb_get_pixel( GdkPixbuf* pb, int x, int y )
// {
//   guchar* pixels = gdk_pixbuf_get_pixels(pb);
//   int rs = gdk_pixbuf_get_rowstride(pb);
//   int ch = gdk_pixbuf_get_n_channels(pb);
//   return( pixels + y * rs + x * ch );
// }

static guchar* pb_get_pixel( Fl_Shared_Image* img, int x, int y )
{
  guchar* pixels = (guchar*)(img->data()[0]);
  //int rs = gdk_pixbuf_get_rowstride(pb);
  //int ch = gdk_pixbuf_get_n_channels(pb);

  //unsigned int depth = img->d();
  //unsigned int width = img->w();
  unsigned int index = (y * img->w() * img->d()) + (x * img->d());
  return( pixels + index );
}

static void pb_set_pixel( Fl_Shared_Image* pb, int x, int y, uint8_t val )
{
  // bounds checking
  int width = pb->w();//gdk_pixbuf_get_width(pb);
  int height = pb->h();//gdk_pixbuf_get_height(pb);
  if( x >=0 && x < width && y >= 0 && y < height )
    {
      // zeroing
      guchar* pix = pb_get_pixel( pb, x, y );
      unsigned int bytes_per_sample = 1;//gdk_pixbuf_get_bits_per_sample (pb) / 8;
      unsigned int num_samples = pb->d();//gdk_pixbuf_get_n_channels(pb);
      memset( pix, val, num_samples * bytes_per_sample );
    }
  else
    PRINT_WARN4( "pb_set_pixel coordinate %d,%d out of range (image dimensions %d by %d)", x, y, width, height );
}

// set all the pixels in a rectangle 
static void pb_set_rect( Fl_Shared_Image* pb, int x, int y, int width, int height, uint8_t val )
 {
   int bytes_per_sample = 1;//gdk_pixbuf_get_bits_per_sample (pb) / 8;
   int num_samples = pb->d();//gdk_pixbuf_get_n_channels(pb);

   int a, b;
   for( a = y; a < y+height; a++ )
     for( b = x; b < x+width; b++ )
       {	
 	// zeroing
 	guchar* pix = pb_get_pixel( pb, b, a );
 	memset( pix, val, num_samples * bytes_per_sample );
       }
 }  

// // returns TRUE if any channel in the pixel is non-zero
static gboolean pb_pixel_is_set( Fl_Shared_Image* img, int x, int y, int threshold )
{
  guchar* pixel = pb_get_pixel( img,x,y );
  return( pixel[0] > threshold );
}

int Stg::stg_rotrects_from_image_file( const char* filename, 
					 stg_rotrect_t** rects, 
					 unsigned int* rect_count,
					 unsigned int* widthp, 
					 unsigned int* heightp )
{
  // TODO: make this a parameter
  const int threshold = 127;
  
  Fl_Shared_Image *img = Fl_Shared_Image::get(filename);
  assert( img );

  //printf( "loaded image %s w %d h %d d %d count %d ld %d\n", 
  //  filename, img->w(), img->h(), img->d(), img->count(), img->ld() );

  *rect_count = 0;
  size_t allocation_unit = 1000;
  size_t rects_allocated = allocation_unit;
  *rects = (stg_rotrect_t*)g_new( stg_rotrect_t, rects_allocated );
  
  int img_width = img->w();//gdk_pixbuf_get_width(pb);
  int img_height = img->h();//gdk_pixbuf_get_height(pb);
  
  // if the caller wanted to know the dimensions
  if( widthp ) *widthp = img_width;
  if( heightp ) *heightp = img_height;
  
  
  int y, x;
  for(y = 0; y < img_height; y++)
    {
      for(x = 0; x < img_width; x++)
	{
	  // skip blank (white) pixels
	  if(  pb_pixel_is_set( img,x,y, threshold) )
	    continue;

	  // a rectangle starts from this point
	  int startx = x;
	  int starty = y;
	  int height = img_height; // assume full height for starters
	  
	  // grow the width - scan along the line until we hit an empty (white) pixel
	  for( ; x < img_width &&  ! pb_pixel_is_set(img,x,y,threshold); x++ )
	    {
	      // handle horizontal cropping
	      //double ppx = x * sx; 
	      //if (ppx < this->crop_ax || ppx > this->crop_bx)
	      //continue;
	      
	      // look down to see how large a rectangle below we can make
	      int yy  = y;
	      while( ! pb_pixel_is_set(img,x,yy,threshold) && (yy < img_height-1) )
		{ 
		  // handle vertical cropping
		  //double ppy = (this->image->height - yy) * sy;
		  //if (ppy < this->crop_ay || ppy > this->crop_by)
		  //continue;
		  
		  yy++; 
		} 	      

	      // now yy is the depth of a line of non-zero pixels
	      // downward we store the smallest depth - that'll be the
	      // height of the rectangle
	      if( yy-y < height ) height = yy-y; // shrink the height to fit
	    } 
	  
	  // whiten the pixels we have used in this rect
	  pb_set_rect( img, startx, starty, x-startx, height, 0xFF );
	  
	  // add this rectangle to the array
	  (*rect_count)++;
	  
	  if( (*rect_count) > rects_allocated )
	    {
	      rects_allocated = (*rect_count) + allocation_unit;
	      
	      *rects = (stg_rotrect_t*)
		realloc( *rects, rects_allocated * sizeof(stg_rotrect_t) );
	    }

	  //  y-invert all the rectangles because we're using conventional
	  // rather than graphics coordinates. this is much faster than
	  // inverting the original image.
	  stg_rotrect_t *latest = &(*rects)[(*rect_count)-1];
	  latest->pose.x = startx;
	  latest->pose.y = img_height - (starty + height);
	  latest->pose.a = 0.0;
	  latest->size.x = x - startx;
	  latest->size.y = height;
	  
	  //printf( "rect %d (%.2f %.2f %.2f %.2f %.2f\n", 
	  //  *rect_count, 
	  //  latest->x, latest->y, latest->a, latest->w, latest->h ); 
	  
	}
    }
  
  if( img ) img->release(); // frees all image resources: the
			  // constructor is not available

  return 0; // ok
}

// void Stg::print_pointer( void* p, char* separator )
// {
//   printf( "%p%s", p, separator) ;
// }


// POINTS -----------------------------------------------------------

stg_point_t* Stg::stg_points_create( size_t count )
{
  return( (stg_point_t*)g_new( stg_point_t, count ));
}

void Stg::stg_points_destroy( stg_point_t* pts )
{
  g_free( pts );
}


stg_point_t* Stg::stg_unit_square_points_create( void )
{
  stg_point_t * pts = stg_points_create( 4 );
  
  pts[0].x = 0;
  pts[0].y = 0;
  pts[1].x = 1;
  pts[1].y = 0;
  pts[2].x = 1;
  pts[2].y = 1;
  pts[3].x = 0;
  pts[3].y = 1;  
  
  return pts;
}


// CALLBACKS -------------------------------------------------------

stg_cb_t* Stg::cb_create( stg_model_callback_t callback, void* arg )
{
  stg_cb_t* cb = (stg_cb_t*)g_new( stg_cb_t, 1 );
  cb->callback = callback;
  cb->arg = arg;
  return cb;
}

void Stg::cb_destroy( stg_cb_t* cb )
{
  free( cb );
}

// return a value based on val, but limited minval <= val >= maxval  
double Stg::constrain( double val, double minval, double maxval )
{
  if( val < minval )
    return minval;

  if( val > maxval )
    return maxval;
  
  return val;
}


stg_color_t Stg::stg_color_pack( double r, double g, double b, double a )
{
  stg_color_t col=0;
  col += (stg_color_t)((1.0-a)*255.0)<<24;
  col += (stg_color_t)(r*255.0)<<16;
  col += (stg_color_t)(g*255.0)<<8;
  col += (stg_color_t)(b*255.0);

  return col;
}

void Stg::stg_color_unpack( stg_color_t col, 
		       double* r, double* g, double* b, double* a )
{
  if(a) *a = 1.0 - (((col & 0xFF000000) >> 24) / 255.0);
  if(r) *r = ((col & 0x00FF0000) >> 16) / 255.0;
  if(g) *g = ((col & 0x0000FF00) >> 8)  / 255.0;
  if(b) *b = ((col & 0x000000FF) >> 0)  / 255.0;
}


// DRAW INTERFACE

using namespace Draw;

draw_t* Draw::create( type_t type,
		      vertex_t* verts,
		      size_t vert_count )
{
  size_t vert_mem_size = vert_count * sizeof(vertex_t);
  
  // allocate space for the draw structure and the vertex data behind it
  draw_t* d = (draw_t*)
    g_malloc( sizeof(draw_t) + vert_mem_size );
  
  d->type = type;
  d->vert_count = vert_count;
  
  // copy the vertex data behind the draw structure
  memcpy( d->verts, verts, vert_mem_size );
	     
  return d;
}

void Draw::destroy( draw_t* d )
{
  g_free( d );
}