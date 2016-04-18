#include "raster_request_handler.hpp"
#include "request.hpp"
#include "reply.hpp"
#include "geom_helpers.hpp"
#include "logger.hpp"
#include "base64.hpp"

#include <fstream>
#include <png.h>

using namespace std ;
namespace fs = boost::filesystem ;
using namespace http ;

const boost::regex RasterRequestHandler::uri_pattern_(R"(/map/([^/]+)/tiles/([^/]+)/(\d+)/(\d+)/(\d+)\.png)") ;

const uint64_t raster_tile_cache_size = 20*1024*1024 ;

RasterTileCache RasterRequestHandler::tile_cache_(raster_tile_cache_size) ;

RasterRequestHandler::RasterRequestHandler(const string &id, const string &tileSet):
    tileset_(tileSet), map_id_(id), provider_(&tile_cache_)
{
    if ( !provider_.open(tileset_.native()) ) {
        LOG_FATAL_STREAM("Error opening raster tileset: " << tileset_) ;
    }
}

static void png_write_callback(png_structp  png_ptr, png_bytep data, png_size_t length) {
    std::vector<uint8_t> *p = (std::vector<uint8_t>*)png_get_io_ptr(png_ptr);
    p->insert(p->end(), data, data + length);
}

static bool save_png(uint8_t *pixels, int w, int h, vector<uint8_t> &data)
{
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);

    if ( !png ) return false;

    png_infop info = png_create_info_struct(png);

    if (!info) {
        png_destroy_write_struct(&png, &info);
        return false;
    }

    if (setjmp(png_jmpbuf(png)))
    {
        png_destroy_write_struct(&png, &info);
        return false ;
    }

    png_set_IHDR(png, info, w, h, 8 /* depth */, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_set_packing(png);

    png_bytep rows[h] ;

    for (int i = 0; i < h; ++i)
        rows[i] = (png_bytep)(pixels + i * w * 4);

    png_set_rows(png, info, &rows[0]);
    png_set_write_fn(png, (png_voidp)&data, png_write_callback, NULL);
    png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);
    png_destroy_write_struct(&png, &info);

    return true ;
}


static const char *g_empty_transparent_png_256 =
        "iVBORw0KGgoAAAANSUhEUgAAAQAAAAEACAYAAABccqhmAAAABGdBTUEAALGPC/xhBQAAAAFzUkdC\
        AK7OHOkAAAAgY0hSTQAAeiYAAICEAAD6AAAAgOgAAHUwAADqYAAAOpgAABdwnLpRPAAAAAZiS0dE\
        AP8A/wD/oL2nkwAAAAlwSFlzAAAASAAAAEgARslrPgAAARVJREFUeNrtwTEBAAAAwqD1T+1rCKAA\
        AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\
        AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\
        AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\
        AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\
        AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAHgDATwAAdgpQwQAAAAldEVYdGRhdGU6Y3JlYXRlADIw\
        MTYtMDQtMDFUMDk6MTE6MjMrMDM6MDCHKZUkAAAAJXRFWHRkYXRlOm1vZGlmeQAyMDE2LTA0LTAx\
        VDA5OjExOjIzKzAzOjAw9nQtmAAAAABJRU5ErkJggg==" ;

void RasterRequestHandler::handle_request(const Request &request, Response &resp)
{
        boost::smatch m ;
        boost::regex_match(request.path_, m, uri_pattern_) ;

        int zoom = stoi(m.str(3)) ;
        int tx = stoi(m.str(4)) ;
        int ty = stoi(m.str(5)) ;

        ty = pow(2, zoom) - 1 - ty ;

        LOG_INFO_STREAM("Recieved request for raster tile: (" << tx << '/' << ty << '/' << zoom << ")" << "of map " << m.str(1) << "-" << m.str(2)) ;

        time_t mod_time = boost::filesystem::last_write_time(tileset_.native());

        BBox box ;
        tms::tileBounds(tx, ty, zoom, box.minx_, box.miny_, box.maxx_, box.maxy_) ;

        float scale = provider_.georef_[0] ;
        float C = provider_.georef_[4] ;
        float F = provider_.georef_[5] ;

        float x0 = (box.minx_ - C)/scale ;
        float y1 = -(box.miny_ - F)/scale ;
        float x1 = (box.maxx_ - C)/scale ;
        float y0 = -(box.maxy_ - F)/scale ;

        std::unique_ptr<uint8_t []> buffer(new uint8_t [256*256*4]) ;
        memset(buffer.get(), 0, 256*256*4) ;

        if ( provider_.read(x0, y0, x1-x0, y1-y0, 256, 256, buffer.get(), 256*4) )
        {
            vector<uint8_t> data ;
            save_png(buffer.get(), 256, 256, data) ;

            resp.encode_file_data(std::string(data.begin(), data.end()), string(), "image/png", mod_time) ;

        }
        else {
            string empty_tile = base64_decode(g_empty_transparent_png_256) ;
            resp.encode_file_data(empty_tile, string(), "image/png", mod_time) ;
        }
}
