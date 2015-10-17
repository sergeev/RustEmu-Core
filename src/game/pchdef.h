// add here most rarely modified headers to speed up debug build compilation
#include "Common.h"

#include <boost/config.hpp>
#include <boost/version.hpp>
#include <boost/asio.hpp>
#include <boost/atomic.hpp>
#include <boost/bind.hpp>
#include <boost/date_time.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>
#include <boost/scoped_array.hpp>
#include <boost/thread.hpp>
#include <boost/thread/tss.hpp>

#include "MapManager.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "SQLStorages.h"
#include "Opcodes.h"
#include "SharedDefines.h"
#include "GuildMgr.h"
#include "ObjectMgr.h"
#include "ScriptMgr.h"
#include "WorldObjectEvents.h"
#include "WorldLocation.h"
#include "WorldStateMgr.h"
#include "LFGMgr.h"
