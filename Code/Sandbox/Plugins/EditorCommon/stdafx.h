// Copyright 2001-2018 Crytek GmbH / Crytek Group. All rights reserved.

#pragma once

#include <CryCore/Project/CryModuleDefs.h>
#include <CryCore/Platform/platform.h>

#define CRY_USE_MFC
#define CRY_SUPPRESS_CRYENGINE_WINDOWS_FUNCTION_RENAMING
#include <CryCore/Platform/CryAtlMfc.h>

#include <CryExtension/CryGUID.h>
#include <QMetaType>

Q_DECLARE_METATYPE(CryGUID);


#ifdef USE_PCH
	#include <QAbstractItemModel>
	#include <QAction>
	#include <QApplication>
	#include <QButtonGroup>
	#include <QCheckBox>
	#include <QColor>
	#include <QComboBox>
	#include <QDir>
	#include <QDrag>
	#include <QFileDialog>
	#include <QFileInfo>
	#include <QFrame>
	#include <QGridLayout>
	#include <QHBoxLayout>
	#include <QHeaderView>
	#include <QLabel>
	#include <QLineEdit>
	#include <QListView>
	#include <QMap>
	#include <QMenu>
	#include <QMimeData>
	#include <QMouseEvent>
	#include <QObject>
	#include <QPointer>
	#include <QPushButton>
	#include <QRadioButton>
	#include <QSortFilterProxyModel>
	#include <QSpacerItem>
	#include <QStackedWidget>
	#include <QString>
	#include <QStringList>
	#include <QStyle>
	#include <QTabWidget>
	#include <QTimer>
	#include <QToolButton>
	#include <QToolTip>
	#include <QTreeView>
	#include <QValidator>
	#include <QVariant>
	#include <QVBoxLayout>
	#include <QVector>
	#include <QWidget>

	#ifdef USE_PYTHON_SCRIPTING
		#include <CryCore/BoostHelpers.h>
		#include <boost/python.hpp>
		#include <CryCore/Assert/CryAssert.h>
	#endif

	#include <algorithm>
	#include <array>
	#include <atomic>
	#include <bitset>
	#include <cassert>
	#include <ccomplex>
	#include <cctype>
	#include <cerrno>
	#include <cfenv>
	#include <cfloat>
	#include <chrono>
	#include <cinttypes>
	#include <ciso646>
	#include <climits>
	#include <clocale>
	#include <cmath>
	#include <codecvt>
	#include <complex>
	#include <condition_variable>
	#include <csetjmp>
	#include <csignal>
	#include <cstdalign>
	#include <cstdarg>
	#include <cstdbool>
	#include <cstddef>
	#include <cstdint>
	#include <cstdio>
	#include <cstdlib>
	#include <cstring>
	#include <ctgmath>
	#include <ctime>
	#include <cwchar>
	#include <cwctype>
	#include <deque>
	#include <exception>
	#include <forward_list>
	#include <fstream>
	#include <functional>
	#include <future>
	#include <initializer_list>
	#include <iomanip>
	#include <ios>
	#include <iosfwd>
	#include <iostream>
	#include <istream>
	#include <iterator>
	#include <limits>
	#include <list>
	#include <locale>
	#include <map>
	#include <memory>
	#include <mutex>
	#include <new>
	#include <numeric>
	#include <ostream>
	#include <queue>
	#include <random>
	#include <ratio>
	#include <regex>
	#include <scoped_allocator>
	#include <set>
	#include <sstream>
	#include <stack>
	#include <stdexcept>
	#include <streambuf>
	#include <string>
	#include <system_error>
	#include <thread>
	#include <tuple>
	#include <type_traits>
	#include <typeindex>
	#include <typeinfo>
	#include <unordered_map>
	#include <unordered_set>
	#include <utility>
	#include <valarray>
	#include <vector>

	#include <CrySerialization/yasli/Archive.h>
	#include <CrySerialization/yasli/STL.h>

#endif // USE_PCH
