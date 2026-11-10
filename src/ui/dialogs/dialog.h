#pragma once

#include <memory>
#include <optional>
#include <typeinfo>
#include <utility>

namespace andromeda::app {

class ImageResources;

// refactor: the pre-registry framework used to live here as a commented-out block
// (dialog_flags, dialog_names, DialogArgs, DialogAction/DialogOpen/DialogClose, MaybeDlgAction,
// DlgButtonAction and the old Dialog base with init_dialog/get_dialog_name/get_action_buttons).
// It is deleted rather than kept beside the live code: a block that no longer compiles is what
// makes references look mangled. DialogPayload carries the open argument, DialogActions names
// the button set, and DialogRegistry/DialogPresenter own construction and drawing.

/// Footer buttons a dialog presents.
enum class DialogActions {
    None,
    Ok,
    OkCancel,
    Cancel,
    YesNo
};

/// Type-erased argument handed to a dialog when it opens.
/// refactor: replaces `using DialogArgs = std::vector<std::any>`, where every dialog had to
/// guess which vector element was its own and a wrong guess read the wrong type silently. A
/// payload holds exactly one value and the probe is typed, so a mismatched open is detectable.
class DialogPayload {
public:
    DialogPayload() = default;

    /// @tparam T payload type; stored by value.
    /// @param value the option struct the target dialog understands.
    template <typename T>
    explicit DialogPayload(T value)
        : value_(std::make_shared<const Holder<T>>(std::move(value))) { }

    /// @return true when a value was supplied.
    [[nodiscard]] bool has_value() const noexcept { return value_ != nullptr; }

    /// @tparam T requested payload type.
    /// @return pointer to the stored value, or nullptr when the payload holds another type
    ///         (or nothing at all).
    template <typename T>
    [[nodiscard]] const T* get_if() const noexcept {
        if (value_ == nullptr) {
            return nullptr;
        }
        return static_cast<const T*>(value_->probe(typeid(T)));
    }

private:
    /// Type-erased cell; `probe` is the only virtual step, so the type check is a single compare.
    struct Base {
        virtual ~Base() = default;
        /// @param type requested type.
        /// @return address of the stored value when @p type matches, nullptr otherwise.
        [[nodiscard]] virtual const void* probe(const std::type_info& type) const noexcept = 0;
    };

    template <typename T>
    struct Holder final : Base {
        explicit Holder(T incoming) : value(std::move(incoming)) { }

        [[nodiscard]] const void* probe(const std::type_info& type) const noexcept override {
            return type == typeid(T) ? &value : nullptr;
        }

        T value;
    };

    std::shared_ptr<const Base> value_{};
};

/// Base of every dialog.
/// A subclass paints content only: the base owns the window, the footer buttons implied by
/// get_actions() and the close transition. One instance per registered type is reused, so
/// on_open() must reset any per-open state.
class Dialog {
public:
    Dialog() = default;
    virtual ~Dialog() = default;

    Dialog(const Dialog&) = delete;
    Dialog& operator=(const Dialog&) = delete;

    /// Runs one frame: puts a pending open request on screen, paints content and footer, applies a
    /// close. Safe to call only for a dialog that wants_open(); the presenter guarantees that.
    void draw();

    /// Requests the dialog to open on the next draw.
    /// refactor: the payload is applied here and not on the next draw. A caller that opened a
    /// dialog and immediately asked it what it needs (run_plugin() reading needs_dialog()) used to
    /// observe the state from before on_open(), which is why plugin dialogs never appeared.
    /// on_open() must therefore stay a pure state step; painting still happens in draw().
    /// @param payload argument handed to on_open() before this call returns.
    void open(const DialogPayload& payload = DialogPayload{ });

    /// Requests the dialog to close. Applied on the next draw, so a caller never closes a window
    /// from outside its own frame.
    void close();

    /// @return true while the dialog is on screen.
    [[nodiscard]] bool is_opened() const noexcept { return is_open_; }

    /// refactor: the presentation state is owned here, not read back from ImGui. The presenter
    /// asks this to decide whether to draw at all; a freshly open()ed instance answers yes before
    /// its first frame, which is what makes the very first open work.
    /// @return true when an open request is pending or the dialog is on screen.
    [[nodiscard]] bool wants_open() const noexcept { return open_request_ || is_open_; }

    /// @return true when the last time the dialog closed, it closed by confirming. Cleared by
    ///         the next open(), so a cancel never fires a confirm handler.
    [[nodiscard]] bool was_confirmed() const noexcept { return confirmed_; }

    /// Clears the confirmed latch; the presenter calls this after running the handler so one
    /// confirmation is delivered exactly once.
    void consume_confirmation() noexcept { confirmed_ = false; }

    [[nodiscard]] virtual const char* get_title() const = 0;
    virtual void draw_content() = 0;

    /// Applies the argument the opener supplied. Called from open(), before any frame is drawn, so
    /// a subclass may assume its state is ready as soon as open() returns.
    virtual void on_open(const DialogPayload& payload) { (void)payload; }
    virtual void on_register() {}
    virtual void on_click_confirm() {}
    virtual void on_click_cancel() {}

    [[nodiscard]] virtual const DialogActions get_actions() const { return DialogActions::Ok; }

    /// Installs the shared image handle; the presenter does this once before the first frame.
    /// @param resources image/font set owned elsewhere; must outlive the dialog.
    void set_image_resources(const ImageResources& resources) noexcept { images_ = &resources; }

protected:
    /// @return the shared image handle.
    [[nodiscard]] const ImageResources& images() const noexcept { return *images_; }

    /// Paints the footer for the actions get_actions() reports.
    /// @return the action the user pressed, or std::nullopt when none was.
    [[nodiscard]] std::optional<DialogActions> draw_actions();

    bool open_request_ = false;
    bool close_request_ = false;
    bool is_open_ = false;
    bool confirmed_ = false;
    const ImageResources* images_ = nullptr;
};

} // namespace andromeda::app
