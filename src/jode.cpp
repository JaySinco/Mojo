#include "resource.h"
#include "context.h"
#include "platform.h"
#include <uv.h>
#include <boost/algorithm/string.hpp>
#include <filesystem>

DEFINE_bool(e, false, "evaluate code snippet");
DEFINE_int32(thread_num, 4, "node thread pool size");
DEFINE_string(node_flags, "", "flags used by node, seperated by ';'");
DEFINE_string(rpath, ".", "require path");

int node_run(node::MultiIsolatePlatform *platform, const std::vector<std::string> &args,
             const std::vector<std::string> &exec_args)
{
    int exit_code = 0;
    uv_loop_t loop;
    if (int ret = uv_loop_init(&loop); ret != 0) {
        LOG(ERROR) << "failed to init uv loop: {}"_format(uv_err_name(ret));
        return 1;
    }
    std::shared_ptr<node::ArrayBufferAllocator> allocator = node::ArrayBufferAllocator::Create();
    v8::Isolate *isolate = NewIsolate(allocator.get(), &loop, platform);
    if (isolate == nullptr) {
        LOG(ERROR) << "failed to create v8 isolate";
        return 1;
    }
    {
        v8::Locker locker(isolate);
        v8::Isolate::Scope isolate_scope(isolate);
        std::unique_ptr<node::IsolateData, decltype(&node::FreeIsolateData)> isolate_data(
            node::CreateIsolateData(isolate, &loop, platform, allocator.get()),
            node::FreeIsolateData);
        v8::HandleScope handle_scope(isolate);
        v8::Local<v8::Context> context = create_context(isolate);
        if (context.IsEmpty()) {
            LOG(ERROR) << "failed to create v8 context";
            return 1;
        }
        v8::Context::Scope context_scope(context);
        std::unique_ptr<node::Environment, decltype(&node::FreeEnvironment)> env(
            node::CreateEnvironment(isolate_data.get(), context, args, exec_args),
            node::FreeEnvironment);
        auto [ok, size, rc_data] = load_rc_file(MAKEINTRESOURCE(IDC_BOOTSTRAP_JS));
        if (!ok || rc_data == nullptr) {
            LOG(ERROR) << "failed to load bootstrap code";
            return 1;
        }
        std::string code(rc_data, size);
        VLOG(3) << "bootstrap-code=\n" << code;
        v8::MaybeLocal<v8::Value> loadenv_ret = node::LoadEnvironment(env.get(), code.c_str());
        if (loadenv_ret.IsEmpty()) {
            LOG(ERROR) << "failed to load node env";
            return 1;
        }
        {
            v8::SealHandleScope seal(isolate);
            bool more;
            do {
                uv_run(&loop, UV_RUN_DEFAULT);
                platform->DrainTasks(isolate);
                more = uv_loop_alive(&loop);
                if (more) {
                    continue;
                }
                node::EmitBeforeExit(env.get());
                more = uv_loop_alive(&loop);
            } while (more == true);
        }
        exit_code = node::EmitExit(env.get());
        node::Stop(env.get());
    }
    bool platform_finished = false;
    platform->AddIsolateFinishedCallback(
        isolate, [](void *data) { *static_cast<bool *>(data) = true; }, &platform_finished);
    platform->UnregisterIsolate(isolate);
    isolate->Dispose();
    while (!platform_finished) {
        uv_run(&loop, UV_RUN_ONCE);
    }
    if (int err = uv_loop_close(&loop); err != 0) {
        LOG(ERROR) << "failed to close uv loop";
    }
    return exit_code;
}

int node_main(int argc, char **argv)
{
    argv = uv_setup_args(argc, argv);
    std::vector<std::string> args(argv, argv + argc);
    std::vector<std::string> exec_args;
    std::vector<std::string> errors;
    int exit_code = node::InitializeNodeWithArgs(&args, &exec_args, &errors);
    for (const std::string &error : errors) {
        LOG(ERROR) << "failed to init node: {}"_format(error.c_str());
    }
    if (exit_code != 0) {
        return exit_code;
    }
    std::unique_ptr<node::MultiIsolatePlatform> platform =
        node::MultiIsolatePlatform::Create(FLAGS_thread_num);
    v8::V8::InitializePlatform(platform.get());
    v8::V8::Initialize();
    exit_code = node_run(platform.get(), args, exec_args);
    v8::V8::Dispose();
    v8::V8::ShutdownPlatform();
    return exit_code;
}

int main(int argc, char **argv)
{
    INIT_LOG(argc, argv);
    // resolve rpath
    std::wstring rpath;
    if (FLAGS_rpath.size() == 0) {
        wchar_t cwd[MAX_PATH] = {0};
        GetModuleFileNameW(NULL, cwd, MAX_PATH);
        rpath = std::filesystem::path(cwd).parent_path().generic_wstring();
    } else {
        auto path = std::filesystem::path(FLAGS_rpath);
        if (path.is_relative()) {
            path = std::filesystem::absolute(path);
        }
        rpath = path.generic_wstring();
    }
    g_injected.rpath = ws2s(rpath, CP_UTF8);
    // resolve code & filename
    if (argc < 2) {
        auto [ok, size, rc_data] = load_rc_file(MAKEINTRESOURCE(IDC_REPL_JS));
        if (!ok || rc_data == nullptr) {
            LOG(ERROR) << "failed to load repl code";
            return 1;
        }
        g_injected.code = std::string(rc_data, size);
        g_injected.filename = "repl.js";
    } else {
        std::wstring warg1 = s2ws(argv[1]);
        if (FLAGS_e) {
            g_injected.code = ws2s(warg1, CP_UTF8);
            g_injected.filename = "<anonymous>";
        } else {
            auto [ok, code] = read_file(warg1);
            g_injected.code = code;
            g_injected.filename = ws2s(warg1, CP_UTF8);
        }
    }
    if (g_injected.code.size() == 0) {
        return 1;
    }
    // resolve node argument
    VLOG(3) << "filename=" << g_injected.filename;
    VLOG(3) << "code=\n" << g_injected.code;
    VLOG(3) << "rpath=" << g_injected.rpath;
    int node_argc = 1;
    std::vector<char *> node_argv = {argv[0]};
    if (argc < 2) {
        ++node_argc;
        node_argv.push_back(new char[]{"--experimental-repl-await"});
    }
    std::vector<std::string> argv_buffer;
    if (FLAGS_node_flags.size() > 0) {
        boost::split(argv_buffer, FLAGS_node_flags, boost::is_any_of(";"));
        for (auto &a : argv_buffer) {
            ++node_argc;
            node_argv.push_back(a.data());
        }
    }
    for (int i = 0; i < node_argc; ++i) {
        VLOG(3) << "arg[" << i << "]=" << node_argv.at(i);
    }
    return node_main(node_argc, node_argv.data());
}
