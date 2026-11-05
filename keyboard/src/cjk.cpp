#include "framekeyboard/cjk.hpp"
#include "cjk_engine.hpp"

#include <dlfcn.h>
#include <filesystem>
#include <map>
#include <stdexcept>

namespace framekeyboard {
namespace {
struct Module {
    void* handle{};
    CreateCjkEngine create{};
    explicit Module(const std::string& name) {
        const auto executable = std::filesystem::read_symlink("/proc/self/exe").parent_path();
        const auto filename = "framekeyboard-" + name + ".so";
        // Only load sibling modules, never working-directory or system-path
        // lookalikes. Development builds use engines/; packages use lib/.
        auto path = executable / "engines" / filename;
        if (!std::filesystem::exists(path)) {
            path = executable.parent_path() / "lib/framekeyboard" / filename;
        }
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            throw std::runtime_error(name + " engine unavailable. Choose another language.");
        }
        create = reinterpret_cast<CreateCjkEngine>(dlsym(handle, "framekeyboard_create_cjk_v1"));
        if (!create) {
            dlclose(handle);
            throw std::runtime_error(name + " engine module is incompatible. Reinstall this release.");
        }
    }
    ~Module() { dlclose(handle); }
};
std::shared_ptr<Module> load(const std::string& method) {
    const std::string name =
        method == "korean-2set"                                                           ? "hangul"
        : method == "chinese-pinyin-simplified" || method == "chinese-pinyin-traditional" ? "pinyin"
                                                                                          : "";
    if (name.empty()) {
        throw std::runtime_error("Unknown Chinese/Korean input method");
    }
    // Applying a profile briefly keeps old and new contexts alive together.
    // Share their module, then unload after its last context is destroyed.
    static std::map<std::string, std::weak_ptr<Module>> modules;
    auto module = modules[name].lock();
    if (!module) {
        module = std::make_shared<Module>(name);
        modules[name] = module;
    }
    return module;
}
} // namespace
struct CjkComposer::State {
    std::shared_ptr<Module> module;
    std::unique_ptr<CjkEngine> engine; // Reverse destruction keeps module code alive.
    explicit State(const std::string& method)
        : module(load(method)), engine(module->create(method.c_str())) {
        if (!engine) {
            throw std::runtime_error("Could not create composition engine");
        }
    }
};
bool CjkComposer::supported(const std::string& method) {
    try {
        return static_cast<bool>(load(method));
    } catch (const std::exception&) {
        return false;
    }
}
CjkComposer::CjkComposer(const std::string& method) : state_(std::make_unique<State>(method)) {
}
CjkComposer::~CjkComposer() = default;
void CjkComposer::type(char key) {
    state_->engine->type(key);
}
void CjkComposer::backspace() {
    state_->engine->backspace();
}
void CjkComposer::cancel() {
    state_->engine->cancel();
}
void CjkComposer::cycle(int direction) {
    state_->engine->cycle(direction);
}
void CjkComposer::choose(int index) {
    state_->engine->choose(index);
}
std::string CjkComposer::preedit() const {
    return state_->engine->preedit();
}
std::string CjkComposer::text() const {
    return state_->engine->text();
}
std::vector<std::string> CjkComposer::candidates() const {
    return state_->engine->candidates();
}
int CjkComposer::selected() const {
    return state_->engine->selected();
}
bool CjkComposer::empty() const {
    return state_->engine->empty();
}
bool CjkComposer::has_reading() const {
    return state_->engine->has_reading();
}
bool CjkComposer::chinese() const {
    return state_->engine->chinese();
}
} // namespace framekeyboard
