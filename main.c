#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <wayland-server.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <xkbcommon/xkbcommon.h>

// waylandサーバー
struct morning_server {

    // waylandディスプレイ
    struct wl_display *display;
    // デバイスとの入出力を抽象化 (DRM, libinput, X11など)
    struct wlr_backend *backend;
    // レンダリングを抽象化 (Pixman, Vulkan, OpenGLなど)
    struct wlr_renderer *renderer;
    // レンダリング用のバッファを確保
    struct wlr_allocator *allocator;
    // 複数の出力デバイス(モニター)の物理的な位置関係を管理
    struct wlr_output_layout *output_layout;
    // scene graphを管理
    struct wlr_scene *scene;
    // カーソルを管理
    struct wlr_cursor *cursor;
    // カーソルのテーマを管理
    struct wlr_xcursor_manager *xcursor_manager;

    // 新しい入力デバイスの接続を検知するlistener
    struct wl_listener new_input;
    // 新しい出力デバイスの接続を検知するlistener
    struct wl_listener new_output;
    // カーソルのイベントを検知するlistener
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_button;
    struct wl_listener cursor_frame;

    // 認識されたキーボードのリスト
    struct wl_list keyboards;
    // 出力デバイス(モニター)のリスト
    struct wl_list outputs;
};

// 出力デバイスに関するイベントの情報を保持する構造体
struct morning_output {

    // waylandサーバー
    struct morning_server *server;
    // 出力デバイスの情報
    struct wlr_output *wlr_output;

    // フレームの更新を検知するlistener
    struct wl_listener frame;
    // サーバーの終了を検知するlistener
    struct wl_listener destroy;

    // リスト用
    struct wl_list link;
};

// キーボードに関するイベントの情報を保持する構造体
struct morning_keyboard {

    // waylandサーバー
    struct morning_server *server;
    // キーボードの情報
    struct wlr_keyboard *wlr_keyboard;

    // キーボードからの入力を検知するlistener
    struct wl_listener input;
    // サーバーの終了を検知するlistener
    struct wl_listener destroy;

    // リスト用
    struct wl_list link;
};

// Altキーによるキーバインドを処理
static int handle_keybinding_alt(struct morning_server *server, xkb_keysym_t sym) {
    printf("detected keybinding [alt]\n");
    switch (sym) {
    // Alt + Escape: 終了
    case XKB_KEY_Escape:
        wl_display_terminate(server->display);
        return 1;
    default:
        return 0;
    }
}

// キーボードからの入力を受け取ったときのイベント
static void handle_keyboard_input(struct wl_listener *listener, void *data) {
    printf("detected keyboard input\n");
    struct morning_keyboard *keyboard = wl_container_of(listener, keyboard, input);
    struct wlr_keyboard_key_event *event = data;

    // libinputのキーコードをXKBのキーコードに変換
    uint32_t keycode = event->keycode + 8;

    // XKBのキーコードからキーのシンボルを取得 (nsymsはシンボルの数)
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);

    // 押されている修飾キーを取得
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
    int handled = 0;

    // 各シンボルの内容を解釈し、キーバインドを処理
    for (int i = 0; i < nsyms; i++) {
        switch (event->state) {
        case WL_KEYBOARD_KEY_STATE_PRESSED:
            // Alt + ?
            if (modifiers & WLR_MODIFIER_ALT)
                handled = handle_keybinding_alt(keyboard->server, syms[i]);
            break;
        default:
            break;
        }
    }
}

// サーバーの終了を検知したときのキーボードに関するイベント
static void handle_keyboard_destroy(struct wl_listener *listener, void *data) {
    printf("detected keyboard destroy\n");
    struct morning_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    wl_list_remove(&keyboard->input.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

// キーボードの初期化
static void attach_new_keyboard(struct morning_server *server, struct wlr_input_device *device) {

    // input_deviceからキーボードの情報を取得
    struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

    // XKB keymapを適用
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);

    // イベント用のkeyboard構造体を作成
    struct morning_keyboard *keyboard = calloc(1, sizeof(struct morning_keyboard));
    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard;

    // キーボードのイベントを指定
    keyboard->input.notify = handle_keyboard_input;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->input);
    keyboard->destroy.notify = handle_keyboard_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);

    // サーバーのキーボードリストに追加
    wl_list_insert(&server->keyboards, &keyboard->link);
}

// マウスの初期化
static void attach_new_pointer(struct morning_server *server, struct wlr_input_device *device) {
    // カーソルにマウスを関連付ける
    wlr_cursor_attach_input_device(server->cursor, device);
}

// 新しい入力デバイスが接続された時のイベント
static void handle_new_input(struct wl_listener *listener, void *data) {
    struct morning_server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *wlr_input_device = data;

    // デバイスの種類に合わせて初期化
    switch (wlr_input_device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        // キーボード
        attach_new_keyboard(server, wlr_input_device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        // マウス
        attach_new_pointer(server, wlr_input_device);
        break;
    default:
        break;
    }
}

// フレームの更新イベント
static void handle_output_frame(struct wl_listener *listener, void *data) {

    // outputのイベント管理用構造体を取得
    struct morning_output *output = wl_container_of(listener, output, frame);

    // sceneを取得
    struct wlr_scene *scene = output->server->scene;

    // 特定のoutputに対するsceneを取得
    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, output->wlr_output);

    // sceneの内容をoutputにコミット
    wlr_scene_output_commit(scene_output, NULL);
    // 今の時間を取得
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // フレームを更新し、タイムスタンプ
    wlr_scene_output_send_frame_done(scene_output, &now);
}

// サーバーの終了を検知したときの出力デバイスに関するイベント
static void handle_output_destroy(struct wl_listener *listener, void *data) {
    printf("detected output destroy\n");
    struct morning_output *output = wl_container_of(listener, output, destroy);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

// 新しい出力デバイスが接続された時のイベント
static void handle_new_output(struct wl_listener *listener, void *data) {

    // サーバーを取得
    struct morning_server *server = wl_container_of(listener, server, new_output);

    // outputのデータを取得
    struct wlr_output *wlr_output = data;

    // outputのレンダリングサブシステムを初期化
    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    // outputの状態(state)を格納する構造体を初期化
    struct wlr_output_state state;
    wlr_output_state_init(&state);

    // outputの有効化をstateに適用
    wlr_output_state_set_enabled(&state, true);

    // outputのmode (解像度やリフレッシュレートなどの情報) を設定
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != NULL) {
        wlr_output_state_set_mode(&state, mode);
    }

    // outputをoutput_layoutの管理下に追加
    wlr_output_layout_add_auto(server->output_layout, wlr_output);

    // outputのイベント管理用構造体を作成
    struct morning_output *output = calloc(1, sizeof(struct morning_output));
    output->server = server;
    output->wlr_output = wlr_output;

    // outputsに追加
    wl_list_insert(&server->outputs, &output->link);

    // outputのイベントを指定
    output->frame.notify = handle_output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->destroy.notify = handle_output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    // outputの状態をコミット
    // これにより初めて、ディスプレイ上に何かが映し出される
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);
}

// カーソルが動いたときの共通の処理
static void cursor_motion(struct morning_server *server) {
    wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, "default");
}

// カーソルの(クライアントから見た)相対的な動きを検知したときのイベント
static void handle_cursor_motion(struct wl_listener *listener, void *data) {
    printf("detected cursor motion\n");
    struct morning_server *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    cursor_motion(server);
}

// カーソルの絶対的な動きを検知したときのイベント
static void handle_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    printf("detected cursor motion absolute\n");
    struct morning_server *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    cursor_motion(server);
}

// カーソルのホイールの動きを検知したときのイベント
static void handle_cursor_axis(struct wl_listener *listener, void *data) {
    printf("detected cursor axis\n");
}

// カーソルのボタン入力を検知したときのイベント
static void handle_cursor_button(struct wl_listener *listener, void *data) {
    printf("detected cursor button\n");
}

// カーソルのフレームの更新を検知したときのイベント
// このイベントは、カーソルに関する何かしらの入力があったフレームの最後に発生する
static void handle_cursor_frame(struct wl_listener *listener, void *data) {
    printf("detected cursor frame\n");
}

int main(int argc, char *argv[]) {

    // サーバーを初期化
    struct morning_server server = {0};

    // waylandディスプレイを作成
    server.display = wl_display_create();
    assert(server.display);

    // backendを作成
    server.backend = wlr_backend_autocreate(server.display, NULL);
    assert(server.backend);

    // rendererを作成
    server.renderer = wlr_renderer_autocreate(server.backend);
    assert(server.renderer);

    // allocatorを作成
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    assert(server.allocator);

    // output_layoutを作成
    server.output_layout = wlr_output_layout_create();

    // sceneを作成
    server.scene = wlr_scene_create();

    // sceneとoutput_layoutを対応付ける
    wlr_scene_attach_output_layout(server.scene, server.output_layout);

    // cursorを作成
    server.cursor = wlr_cursor_create();

    // cursorとoutput_layoutを対応付ける
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    // xcursor_managerを作成
    server.xcursor_manager = wlr_xcursor_manager_create(NULL, 24);

    // 出力デバイスのリストを作成
    wl_list_init(&server.outputs);

    // キーボードのリストを作成
    wl_list_init(&server.keyboards);

    // 新しい出力デバイスが接続された時のイベントを指定
    server.new_output.notify = handle_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    // 新しい入力デバイスが接続された時のイベントを指定
    server.new_input.notify = handle_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);

    // カーソルのイベントを指定
    server.cursor_motion.notify = handle_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = handle_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
    server.cursor_button.notify = handle_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = handle_cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = handle_cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    // backendを有効化
    if (!wlr_backend_start(server.backend)) {
        fprintf(stderr, "Failed to start backend\n");
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.display);
        return 1;
    }

    // 実行
    wl_display_run(server.display);

    // 終了
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_output_layout_destroy(server.output_layout);
    wlr_xcursor_manager_destroy(server.xcursor_manager);
    wl_display_destroy(server.display);

    return 0;
}