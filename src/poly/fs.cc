/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "poly/fs.h"

#include <string>
#include <vector>
#include <algorithm>

namespace poly {
namespace fs {

std::string CanonicalizePath(const std::string& original_path) {
  char path_separator('\\');
  std::string path(poly::fix_path_separators(original_path, path_separator));

  std::vector<std::string::size_type> path_breaks;

  std::string::size_type pos(path.find_first_of(path_separator));
  std::string::size_type pos_n(std::string::npos);

  while (pos != std::string::npos) {
    if ((pos_n = path.find_first_of(path_separator, pos + 1)) == std::string::npos) {
      pos_n = path.size();
    }

    auto diff(pos_n - pos);
    switch (diff)
    {
    case 0:
      pos_n = std::string::npos;
      break;
    case 1:
      // Duplicate separators
      path.erase(pos, 1);
      pos_n -= 1;
      break;
    case 2:
      // Potential marker for current directory
      if (path[pos + 1] == '.') {
        path.erase(pos, 2);
        pos_n -= 2;
      }
      else {
        path_breaks.push_back(pos);
      }
      break;
    case 3:
      // Potential marker for parent directory
      if (path[pos + 1] == '.' && path[pos + 2] == '.'){
        if (path_breaks.empty()) {
          // Ensure we don't override the device name
          std::string::size_type loc(path.find_first_of(':'));
          auto req(pos + 3);
          if (loc == std::string::npos || loc > req) {
            path.erase(0, req);
            pos_n -= req;
          }
          else {
            path.erase(loc + 1, req - (loc + 1));
            pos_n -= req - (loc + 1);
          }
        }
        else {
          auto last(path_breaks.back());
          auto last_diff((pos + 3) - last);
          path.erase(last, last_diff);
          pos_n = last;
          // Also remove path reference
          path_breaks.erase(path_breaks.end() - 1);
        }
      }
      else {
        path_breaks.push_back(pos);
      }
      break;

    default:
      path_breaks.push_back(pos);
      break;
    }

    pos = pos_n;
  }

  // Remove trailing seperator
  if (!path.empty() && path.back() == path_separator) {
    path.erase(path.size() - 1);
  }

  // Final sanity check for dead paths
  if ((path.size() == 1 && (path[0] == '.' || path[0] == path_separator))
    || (path.size() == 2 && path[0] == '.' && path[1] == '.')) {
    return "";
  }

  return path;
}

WildcardFlags WildcardFlags::FIRST(true, false);
WildcardFlags WildcardFlags::LAST(false, true);

WildcardFlags::WildcardFlags()
  : FromStart(false)
  , ToEnd(false)
  , WithCase(false)
{ }

WildcardFlags::WildcardFlags(bool start, bool end)
  : FromStart(start)
  , ToEnd(end)
  , WithCase(false)
{ }

WildcardRule::WildcardRule(std::string str_match, const WildcardFlags &flags)
  : match(str_match)
  , rules(flags)
{ }

bool WildcardRule::Check(std::string& str) const
{
  if( match.empty() ) {
    return true;
  }

  if( str.size() < match.size() ) {
    return false;
  }

  std::string::size_type result(str.find(match));

  if( result != std::string::npos ) {
    if( rules.FromStart && result != 0 ) {
      return false;
    }

    if( rules.ToEnd && result != ( ( str.size() -1 ) - match.size() ) ) {
      return false;
    }

    str.erase(0, result + match.size());
    return true;
  }

  return false;
}

void WildcardEngine::PreparePattern(const std::string &pattern)
{
  rules.clear();

  WildcardFlags flags(WildcardFlags::FIRST);
  size_t n = 0;
  size_t last = 0;
  while ((n = pattern.find_first_of('*', last)) != pattern.npos) {
    if (last != n) {
      std::string str_str(pattern.substr(last, n - last));
      rules.push_back(WildcardRule(str_str, flags));
    }
    last = n + 1;
    flags = WildcardFlags();
  }
  if (last != pattern.size()) {
    std::string str_str(pattern.substr(last));
    rules.push_back(WildcardRule(str_str, WildcardFlags::LAST));
  }
}

void WildcardEngine::SetRule(const std::string &pattern)
{
  PreparePattern(pattern);
}

bool WildcardEngine::Match(const std::string &str) const
{
  std::string str_copy(str);

  for (const auto& rule : rules) {
    if (!(rule.Check(str_copy))) {
      return false;
    }
  }

  return true;
}

}  // namespace fs
}  // namespace poly
