/*
 * This file is part of theodore (https://github.com/Zlika/theodore).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/* Export marker for the theodore_* functions that a frontend calls on top of
   the standard retro_* API. On Linux/macOS link.T already exports them; on
   Windows they need an explicit dllexport. */
#ifndef __THEODORE_API_H
#define __THEODORE_API_H

#if defined(_WIN32) || defined(__CYGWIN__)
#  define THEODORE_API __declspec(dllexport)
#elif defined(__GNUC__)
#  define THEODORE_API __attribute__((visibility("default")))
#else
#  define THEODORE_API
#endif

#endif /* __THEODORE_API_H */
