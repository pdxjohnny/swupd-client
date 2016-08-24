/*
 *   Software Updater - client side
 *
 *      Copyright © 2015-2016 Intel Corporation.
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, version 2 or later of the License.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#define _GNU_SOURCE // for basename()
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "swupd-build-opts.h"
#include "swupd-internal.h"
#include "swupd.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        return 1;
    }
    init_globals();
    const int version = strtoul(argv[1], NULL, 10);
    setenv("MANIFEST", argv[2], 1);
    struct manifest *MoM = manifest_from_file(version, "MoM");
    if (MoM != NULL) {
        free(MoM);
    } else {
        printf("Failed to load %d MoM manifest\n", version);
    }
    return 0;
}
