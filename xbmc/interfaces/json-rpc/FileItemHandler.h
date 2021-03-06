#pragma once
/*
 *      Copyright (C) 2005-2012 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "JSONRPC.h"
#include "JSONUtils.h"
#include "FileItem.h"
#include "utils/StdString.h"

namespace JSONRPC
{
  class CFileItemHandler : public CJSONUtils
  {
  protected:
    static void FillDetails(ISerializable* info, CFileItemPtr item, const CVariant& fields, CVariant &result);
    static void HandleFileItemList(const char *ID, bool allowFile, const char *resultname, CFileItemList &items, const CVariant &parameterObject, CVariant &result, bool sortLimit = true);
    static void HandleFileItemList(const char *ID, bool allowFile, const char *resultname, CFileItemList &items, const CVariant &parameterObject, CVariant &result, int size, bool sortLimit = true);
    static void HandleFileItem(const char *ID, bool allowFile, const char *resultname, CFileItemPtr item, const CVariant &parameterObject, const CVariant &validFields, CVariant &result, bool append = true);

    static bool FillFileItemList(const CVariant &parameterObject, CFileItemList &list);

    static bool ParseSorting(const CVariant &parameterObject, SortBy &sortBy, SortOrder &sortOrder, SortAttribute &sortAttributes);
    static void ParseLimits(const CVariant &parameterObject, int &limitStart, int &limitEnd);
  private:
    static bool ParseSortMethods(const CStdString &method, const bool &ignorethe, const CStdString &order, SORT_METHOD &sortmethod, SortOrder &sortorder);
    static void Sort(CFileItemList &items, const CVariant& parameterObject);
  };
}
