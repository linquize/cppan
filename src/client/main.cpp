/*
 * Copyright (c) 2016, Egor Pugin
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     1. Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *     2. Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 *     3. Neither the name of the copyright holder nor the names of
 *        its contributors may be used to endorse or promote products
 *        derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <iostream>
#include <thread>
#include <string>

#include <boost/algorithm/string.hpp>

#include <access_table.h>
#include <config.h>
#include <database.h>
#include <http.h>
#include <logger.h>
#include <printers/cmake.h>

#include "build.h"
#include "fix_imports.h"
#include "options.h"
#include "../autotools/autotools.h"

void self_upgrade(Config &c, const char *exe_path);

void default_run()
{
    auto c = Config::get_user_config();
    c.load_current_config();
    c.process();
}

int main(int argc, char *argv[])
try
{
    // initial sequence
    {
        initLogger("info", "", true);

        // initialize CPPAN structures, do not remove
        Config::get_user_config();

        // initialize internal db
        auto &sdb = getServiceDatabase();
        sdb.performStartupActions();
    }

    // default run
    if (argc == 1)
    {
        default_run();
        return 0;
    }

    // command selector
    if (argv[1][0] != '-')
    {
        String cmd = argv[1];

        // internal
        if (cmd == "internal-fix-imports")
        {
            if (argc != 6)
            {
                std::cout << "invalid number of arguments\n";
                std::cout << "usage: cppan internal-fix-imports target aliases.file old.file new.file\n";
                return 1;
            }
            fix_imports(argv[2], argv[3], argv[4], argv[5]);
            return 0;
        }
        if (cmd == "internal-parallel-vars-check")
        {
            if (argc < 6)
            {
                std::cout << "invalid number of arguments: " << argc << "\n";
                std::cout << "usage: cppan internal-parallel-vars-check vars_dir vars_file checks_file generator [toolchain]\n";
                return 1;
            }
            CMakePrinter c;
            if (argc == 6)
                c.parallel_vars_check(argv[2], argv[3], argv[4], argv[5]);
            else if (argc == 7)
                c.parallel_vars_check(argv[2], argv[3], argv[4], argv[5], argv[6]);
            return 0;
        }

        // normal options
        if (cmd == "parse-configure-ac")
        {
            if (argc != 3)
            {
                std::cout << "invalid number of arguments\n";
                std::cout << "usage: cppan parse-configure-ac configure.ac\n";
                return 1;
            }
            process_configure_ac(argv[2]);
            return 0;
        }

        if (cmd == "list")
        {
            auto &db = getPackagesDatabase();
            db.listPackages(argc > 2 ? argv[2] : "");
            return 0;
        }

        if (isUrl(cmd))
            return build(cmd);
        if (fs::exists(cmd))
        {
            if (fs::is_directory(cmd))
            {
                fs::current_path(cmd);
                default_run();
                return 0;
            }
            if (fs::is_regular_file(cmd))
                return build(cmd);
        }

        std::cout << "unknown command\n";
        return 1;
    }
#ifdef _WIN32
    else if (String(argv[1]) == "--self-upgrade-copy")
    {
        // self upgrade via copy
        std::this_thread::sleep_for(std::chrono::seconds(1));
        fs::copy_file(argv[0], argv[2], fs::copy_option::overwrite_if_exists);
        return 0;
    }
#endif
    else if (String(argv[1]) == "--clear-cache")
    {
        CMakePrinter c;
        // TODO: provide better way of opening passed storage in argv[2]
        c.clear_cache();
        return 0;
    }
    else if (String(argv[1]) == "--clear-vars-cache")
    {
        Config c;
        // TODO: provide better way of opening passed storage in argv[2]
        c.clear_vars_cache();
        return 0;
    }

    // default command run

    ProgramOptions options;
    bool r = options.parseArgs(argc, argv);

    // always first
    if (!r || options().count("help"))
    {
        std::cout << options.printHelp() << "\n";
        return !r;
    }
    if (options["version"].as<bool>())
    {
        std::cout << get_program_version_string("cppan") << "\n";
        return 0;
    }

    if (options().count("build"))
        return build(options["build"].as<String>(), options["config"].as<String>());
    else if (options().count("build-only"))
        return build_only(options["build-only"].as<String>(), options["config"].as<String>());
    else if (options().count("rebuild"))
        return build(options["rebuild"].as<String>(), options["config"].as<String>(), true);
    else if (options().count("generate"))
        return generate(options["generate"].as<String>(), options["config"].as<String>());
    else if (options().count("dry-run"))
        return dry_run(options["dry-run"].as<String>(), options["config"].as<String>());
    else if (options().count("build-package"))
        return build_package(options["build-package"].as<String>(), options["settings"].as<String>(), options["config"].as<String>());

    if (options().count(CLEAN_PACKAGES))
    {
        cleanPackages(options[CLEAN_PACKAGES].as<String>());
        return 0;
    }

    // set correct working directory to look for config file
    std::unique_ptr<ScopedCurrentPath> cp;
    if (options().count("dir"))
        cp = std::make_unique<ScopedCurrentPath>(options["dir"].as<std::string>());
    httpSettings.verbose = options["curl-verbose"].as<bool>();
    httpSettings.ignore_ssl_checks = options["ignore-ssl-checks"].as<bool>();

    Config c = Config::get_user_config();
    c.type = ConfigType::Local;

    // setup curl settings if possible from config
    // other network users (options) should go below this line
    httpSettings.proxy = c.settings.proxy;

    // self-upgrade?
    if (options()["self-upgrade"].as<bool>())
    {
        self_upgrade(c, argv[0]);
        return 0;
    }

    // load config from current dir
    c.load_current_config();

    // update proxy settings?
    httpSettings.proxy = c.settings.proxy;

    if (options()["prepare-archive"].as<bool>())
    {
        Projects &projects = c.getProjects();
        for (auto &p : projects)
        {
			auto &project = p.second;
            project.findSources(".");
            String archive_name = make_archive_name(project.ppath.toString());
            if (!project.writeArchive(archive_name))
                throw std::runtime_error("Archive write failed");
        }
    }
    else
    {
        c.process();
    }

    return 0;
}
catch (const std::exception &e)
{
    std::cerr << e.what() << "\n";
    return 1;
}
catch (...)
{
    std::cerr << "Unhandled unknown exception" << "\n";
    return 1;
}

void self_upgrade(Config &c, const char *exe_path)
{
#ifdef _WIN32
    String client = "/client/cppan-master-Windows-client.zip";
#elif __APPLE__
    String client = "/client/cppan-master-macOS-client.zip";
#else
    String client = "/client/.service/cppan-master-Linux-client.zip";
#endif

    DownloadData dd;
    dd.url = c.settings.host + client + ".md5";
    dd.fn = fs::temp_directory_path() / fs::unique_path();
    std::cout << "Downloading checksum file" << "\n";
    download_file(dd);
    auto md5 = boost::algorithm::trim_copy(read_file(dd.fn));

    dd.url = c.settings.host + client;
    dd.fn = fs::temp_directory_path() / fs::unique_path();
    String dl_md5;
    dd.md5.hash = &dl_md5;
    std::cout << "Downloading the latest client" << "\n";
    download_file(dd);
    if (md5 != dl_md5)
        throw std::runtime_error("Downloaded bad file (md5 check failed)");

    std::cout << "Unpacking" << "\n";
    auto tmp_dir = fs::temp_directory_path() / "cppan.bak";
    unpack_file(dd.fn, tmp_dir);

    // self update
    auto program = get_program();
#ifdef _WIN32
    auto exe = (tmp_dir / "cppan.exe").wstring();
    auto arg0 = L"\"" + exe + L"\"";
    auto dst = L"\"" + program.wstring() + L"\"";
    std::cout << "Replacing client" << "\n";
    if (_wexecl(exe.c_str(), arg0.c_str(), L"--self-upgrade-copy", dst.c_str(), 0) == -1)
    {
        throw std::runtime_error(String("errno = ") + std::to_string(errno) + "\n" +
            "Cannot do a self upgrade. Replace this file with newer CPPAN client manually.");
    }
#else
    auto cppan = tmp_dir / "cppan";
    fs::permissions(cppan, fs::owner_all | fs::group_exe | fs::others_exe);
    fs::remove(program);
    fs::copy_file(cppan, program);
    fs::remove(cppan);
#endif
}
