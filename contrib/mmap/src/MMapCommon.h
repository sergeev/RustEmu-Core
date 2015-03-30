/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _MMAP_COMMON_H
#define _MMAP_COMMON_H

#include <string>
#include <vector>
#include <errno.h>
#include <limits.h>
#include <boost/filesystem.hpp>
#include <boost/regex/v4/fileiter.hpp>

#include "Platform/Define.h"

#ifndef WIN32
#include <stddef.h>
#include <dirent.h>
#endif

using namespace std;

namespace MMAP {
  enum ListFilesResult {
    LISTFILE_DIRECTORY_NOT_FOUND,
    LISTFILE_OK
  };

  inline ListFilesResult getDirContents( vector<string>& fileList, string dirpath = ".", 
                                         string filter = "*", bool includeSubDirs = false ) {
    static const char *separator = boost::directory_iterator::separator( );
    string             _path     = dirpath + separator;
    string             _wild     = _path + filter;

    if ( boost::filesystem::exists( _path ) == true ) {
      {
        boost::directory_iterator iterator( _wild.c_str( ) );
        boost::directory_iterator end;
        
        for ( ; iterator != end; ++iterator ) {
          bool is_dir = boost::filesystem::is_directory( iterator.path( ) );
          
          fileList.push_back( iterator.path( ) + _path.length( ) );
          
          if ( ( is_dir == true ) && ( includeSubDirs == true ) ) {
            getDirContents( fileList, iterator.path( ), filter, true );
          }
        }
      }
      
      {
        boost::file_iterator  iterator( _wild.c_str( ) );
        boost::file_iterator  end;
        
        for ( ; iterator != end; ++iterator ) {
          fileList.push_back( iterator.path( ) + _path.length( ) );
        }
      }

      return LISTFILE_OK;
    }

    return LISTFILE_DIRECTORY_NOT_FOUND;
  }
}

#endif
