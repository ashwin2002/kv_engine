/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"

#include "auditevent_generator.h"
#include "generator_event.h"
#include "generator_module.h"
#include "generator_utilities.h"

#include <cJSON_utils.h>
#include <errno.h>
#include <getopt.h>
#include <cstdlib>
#include <iostream>

int main(int argc, char **argv) {
    std::string input_file;
    std::string output_file;
    std::string srcroot;
    std::string objroot;
    int cmd;

    while ((cmd = getopt(argc, argv, "i:r:b:o:")) != -1) {
        switch (cmd) {
        case 'r': /* root */
            srcroot.assign(optarg);
            break;
        case 'b': /* binary root */
            objroot.assign(optarg);
            break;
        case 'o': /* output file */
            output_file.assign(optarg);
            break;
        case 'i': /* input file */
            input_file.assign(optarg);
            break;
        default:
            fprintf(stderr, "usage: %s -r PATH -i FILE -o FILE\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    try {
        unique_cJSON_ptr ptr;
        ptr = load_file(input_file);
        std::list<std::unique_ptr<Module>> modules;
        parse_module_descriptors(ptr.get(), modules, srcroot, objroot);
        create_master_file(modules, output_file);

        for (const auto& module : modules) {
            module->createHeaderFile();
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        exit(EXIT_FAILURE);
    }

    exit(EXIT_SUCCESS);
}
