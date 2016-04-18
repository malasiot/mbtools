#ifndef __MAP_SERVER_H__
#define __MAP_SERVER_H__

#include "server.hpp"

// This creates the map server that will provide tiles and other assets required by mapbox client when displaying a map
// The server responds to the following endpoints:
//
// http://localhost:<port>/map/<mapname>/assets/* (map specific assets such as style file)
// http://localhost:<port>/map/<mapname>/tiles/<tileset>/*/*/*.pbf (vector tiles)
// http://localhost:<port>/map/<mapname>/tiles/<tileset>/*/*/*.png (tiles renderered on the fly or provided from local storage)
// http://localhost:<port>/assets/* (global assets such as fonts, glyphs, js, css)

// On the local filesystem the server expects a fixed structure of file/folder under a root directory provided in the constructor
//
// <root>/maps/
// <root>/maps/<map1>/, <root>/maps/<map2>/ ...
// <root>/maps/<map1>/tiles/<tileset>/<z>/<x>/<y>.(pbf|png) or <root>/maps/<map1>/<tileset>.mbtiles
// <root>/maps/<map1>/tiles/<tileset>.jp2 (raster image in Jpeg2000 format)
// <root>/maps/<map1>/assets/* or <root>/maps/<map1>/assets.sqlite
// <root>/assets/* or <root>/assets.sqlite
//
// The server will search for maps under multiple root paths provided as a ';' delinated list of paths in the
// constructor.
//
// For GPX, KML assets the server supports conversion to GeoJson by appending ?cnv=geojson to the request URL.


class MapServer: public http::Server {
public:
    MapServer(const std::string &root_list, const std::string &ports, bool withgl=true) ;
};

#endif
