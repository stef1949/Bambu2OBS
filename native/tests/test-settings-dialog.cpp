/* Drives the real settings dialog the way a user would, through its widgets. Text
 * comes from the shipped locale file, and a key missing from it fails the test.
 *
 * With no arguments it runs the UI tests. Run by fake_printer.py, which passes
 * <mqtt-port> <ftps-port> <control-port> <fingerprint> <directory>, it runs the
 * certificate trust flow against a real TLS server.
 *
 * Environment: BAMBU_DIALOG_LOCALE is the path to en-US.ini, BAMBU_DIALOG_TEMP a
 * scratch folder, and BAMBU_DIALOG_SCREENSHOT an optional PNG to write. */

#include "bambu-settings.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <obs-module.h>

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

extern "C" {
#include "printer-service.h"
}

#include "test-framework.h"

int bambu_test_failures = 0;

namespace {

constexpr int MAX_KEYS = 512;

struct LocaleEntry {
    char key[96];
    char value[320];
};

LocaleEntry locale_entries[MAX_KEYS];
int locale_count = 0;
int refresh_count = 0;

void load_locale(const char *path)
{
    FILE *file = path != nullptr ? fopen(path, "r") : nullptr;
    char line[512];

    CHECK(file != nullptr);
    if (file == nullptr)
        return;
    while (fgets(line, sizeof(line), file) != nullptr && locale_count < MAX_KEYS) {
        char *equals = strchr(line, '=');
        if (equals == nullptr)
            continue;
        *equals = '\0';
        char *open = strchr(equals + 1, '"');
        char *close = open != nullptr ? strrchr(open + 1, '"') : nullptr;
        if (close == nullptr)
            continue;
        *close = '\0';
        snprintf(locale_entries[locale_count].key, sizeof(locale_entries[0].key), "%s", line);
        snprintf(locale_entries[locale_count].value, sizeof(locale_entries[0].value), "%s", open + 1);
        locale_count++;
    }
    fclose(file);
}

bool load_config(const char *profile_id, struct bambu_service_config *config)
{
    return bambu_profile_store_get(profile_id, &config->profile);
}

/* Deferred deletes only run from an event loop, so a pump has to ask for them. */
void pump()
{
    QApplication::processEvents(QEventLoop::AllEvents, 20);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QThread::msleep(5);
}

template<typename Ready> bool wait_for(Ready ready, int timeout_ms = 20000)
{
    QElapsedTimer timer;

    timer.start();
    while (!ready()) {
        if (timer.elapsed() > timeout_ms)
            return false;
        pump();
    }
    return true;
}

QList<QDialog *> open_dialogs()
{
    QList<QDialog *> result;

    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == "bambuSettings" && widget->isVisible())
            result.append(static_cast<QDialog *>(widget));
    }
    return result;
}

QDialog *wait_for_dialog()
{
    wait_for([]() { return !open_dialogs().isEmpty(); });
    return open_dialogs().isEmpty() ? nullptr : open_dialogs().first();
}

template<typename T> T *control(QDialog *dialog, const char *name)
{
    T *widget = dialog->findChild<T *>(name);
    if (widget == nullptr) {
        fprintf(stderr, "control not found: %s\n", name);
        bambu_test_failures++;
        std::exit(1);
    }
    return widget;
}

void set_field(QDialog *dialog, const char *name, const char *text)
{
    control<QLineEdit>(dialog, name)->setText(QString::fromUtf8(text));
}

void click(QDialog *dialog, const char *name)
{
    control<QPushButton>(dialog, name)->click();
}

bool enabled(QDialog *dialog, const char *name)
{
    return control<QPushButton>(dialog, name)->isEnabled();
}

QString status_text(QDialog *dialog)
{
    return control<QLabel>(dialog, "status")->text();
}

/* Every control sits inside the dialog, and no button is too narrow for its text. */
void check_layout(QDialog *dialog)
{
    const QRect bounds = dialog->rect();

    for (QWidget *child : dialog->findChildren<QWidget *>()) {
        if (!child->isVisible())
            continue;
        const QRect area(child->mapTo(dialog, QPoint(0, 0)), child->size());
        CHECK(bounds.contains(area));
    }
    for (QPushButton *button : dialog->findChildren<QPushButton *>())
        CHECK(button->width() >= button->minimumSizeHint().width());
}

void test_opens_once_on_the_ui_thread()
{
    /* obs-websocket presses buttons from its own threads. */
    std::thread other([]() { bambu_settings_open(); });
    other.join();

    QDialog *dialog = wait_for_dialog();
    CHECK(dialog != nullptr);
    if (dialog == nullptr)
        return;
    CHECK(dialog->thread() == QApplication::instance()->thread());
    CHECK(dialog->isModal());

    bambu_settings_open();
    pump();
    CHECK(open_dialogs().size() == 1);

    click(dialog, "close");
    CHECK(wait_for([]() { return open_dialogs().isEmpty(); }));
}

void test_form()
{
    struct bambu_profile_list profiles;
    char code[32] = "";

    bambu_settings_open();
    QDialog *dialog = wait_for_dialog();
    CHECK(dialog != nullptr);
    if (dialog == nullptr)
        return;
    pump();

    check_layout(dialog);

    /* Nothing selected yet, so the form is disabled. */
    CHECK(!enabled(dialog, "save"));
    CHECK(!control<QLineEdit>(dialog, "host")->isEnabled());

    click(dialog, "addProfile");
    CHECK(enabled(dialog, "save"));
    CHECK(control<QCheckBox>(dialog, "lanOnly")->isChecked());
    CHECK(control<QLineEdit>(dialog, "accessCode")->echoMode() == QLineEdit::Password);
    CHECK(control<QLineEdit>(dialog, "cloudToken")->echoMode() == QLineEdit::Password);

    set_field(dialog, "name", "  Workshop X1C  ");
    set_field(dialog, "host", "127.0.0.1");
    set_field(dialog, "serial", "01S00C000000001");
    set_field(dialog, "accessCode", "87654321");
    click(dialog, "save");

    bambu_profile_store_list(&profiles);
    CHECK(profiles.count == 1);
    CHECK_STR(profiles.profiles[0].name, "Workshop X1C"); /* trimmed */
    CHECK_STR(profiles.profiles[0].host, "127.0.0.1");
    CHECK(profiles.profiles[0].lan_only);
    CHECK(control<QListWidget>(dialog, "profileList")->item(0)->text() == "Workshop X1C");

    /* The secret reached the credential store, and left the form. */
    CHECK(bambu_credential_read(profiles.profiles[0].id, BAMBU_SECRET_ACCESS_CODE, code, sizeof(code)));
    CHECK_STR(code, "87654321");
    CHECK(control<QLineEdit>(dialog, "accessCode")->text().isEmpty());

    /* A blank secret field keeps what is stored. */
    set_field(dialog, "name", "Renamed");
    click(dialog, "save");
    CHECK(bambu_credential_read(profiles.profiles[0].id, BAMBU_SECRET_ACCESS_CODE, code, sizeof(code)));
    CHECK_STR(code, "87654321");

    /* Selecting a profile never reads a stored secret back into the form. */
    click(dialog, "addProfile");
    control<QListWidget>(dialog, "profileList")->setCurrentRow(0);
    CHECK(control<QLineEdit>(dialog, "host")->text() == "127.0.0.1");
    CHECK(control<QLineEdit>(dialog, "accessCode")->text().isEmpty());

    /* Assistive tools select a row without making it current; the form must follow. */
    control<QListWidget>(dialog, "profileList")->item(1)->setSelected(true);
    CHECK(control<QLineEdit>(dialog, "host")->text().isEmpty());
    control<QListWidget>(dialog, "profileList")->item(0)->setSelected(true);
    CHECK(control<QLineEdit>(dialog, "host")->text() == "127.0.0.1");
    control<QListWidget>(dialog, "profileList")->setCurrentRow(1);
    click(dialog, "removeProfile");
    control<QListWidget>(dialog, "profileList")->setCurrentRow(0);

    const char *screenshot = getenv("BAMBU_DIALOG_SCREENSHOT");
    if (screenshot != nullptr) {
        pump();
        CHECK(dialog->grab().save(QString::fromUtf8(screenshot)));
    }

    /* Test Connection against a closed port: reports it, re-enables, sends nothing. */
    bambu_settings_set_ports(1, 1);
    click(dialog, "test");
    CHECK(wait_for([dialog]() { return enabled(dialog, "test"); }));
    CHECK(status_text(dialog).contains(QString::fromUtf8(obs_module_text("BambuOverlay.Settings.Unreachable"))));

    /* Remove wipes the profile and its stored secret. */
    {
        char id[BAMBU_PROFILE_ID_CAPACITY];
        memcpy(id, profiles.profiles[0].id, sizeof(id));
        click(dialog, "removeProfile");
        CHECK(!bambu_credential_exists(id, BAMBU_SECRET_ACCESS_CODE));
    }
    bambu_profile_store_list(&profiles);
    CHECK(profiles.count == 0);
    CHECK(!enabled(dialog, "save"));

    /* Closing tells the sources, whose properties list the printers. */
    const int refreshes = refresh_count;
    click(dialog, "close");
    CHECK(wait_for([]() { return open_dialogs().isEmpty(); }));
    CHECK(refresh_count == refreshes + 1);
}

/* An unroutable address would otherwise keep the worker busy for its full timeout. */
void test_close_cancels_a_running_test()
{
    bambu_settings_open();
    QDialog *dialog = wait_for_dialog();
    CHECK(dialog != nullptr);
    if (dialog == nullptr)
        return;

    click(dialog, "addProfile");
    set_field(dialog, "host", "192.0.2.1");
    bambu_settings_set_ports(BAMBU_DEFAULT_MQTT_PORT, BAMBU_DEFAULT_FTPS_PORT);
    click(dialog, "test");
    CHECK(!enabled(dialog, "test"));

    /* A result lands on the selected profile, so nothing may change it mid-test. */
    CHECK(!control<QListWidget>(dialog, "profileList")->isEnabled());
    CHECK(!enabled(dialog, "addProfile"));
    CHECK(!enabled(dialog, "removeProfile"));
    CHECK(!enabled(dialog, "save"));

    QElapsedTimer timer;
    timer.start();
    click(dialog, "close");
    CHECK(timer.elapsed() < 3000);
    CHECK(wait_for([]() { return open_dialogs().isEmpty(); }));

    /* The profile outlives the dialog; leave nothing behind for later tests. */
    struct bambu_profile_list profiles;
    bambu_profile_store_list(&profiles);
    for (int i = 0; i < profiles.count; i++)
        bambu_profile_store_remove(profiles.profiles[i].id);
}

/* OBS can exit with the dialog open, and the plugin must not leave it behind. */
void test_shutdown_with_dialog_open()
{
    bambu_settings_open();
    CHECK(wait_for_dialog() != nullptr);

    bambu_settings_shutdown();
    bambu_settings_shutdown();
    CHECK(open_dialogs().isEmpty());
    pump();
}

/* How many MQTT logins the fake printer has received: that is, how many times
 * the access code has left the plugin. */
int printer_logins(int control_port)
{
    SOCKET connection = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address = {};
    char reply[32] = "";

    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(control_port));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    CHECK(connect(connection, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) == 0);
    send(connection, "logins\n", 7, 0);
    CHECK(recv(connection, reply, sizeof(reply) - 1, 0) > 0);
    closesocket(connection);
    return atoi(reply);
}

QMessageBox *wait_for_trust_prompt()
{
    QMessageBox *prompt = nullptr;

    wait_for([&prompt]() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == "trustPrompt" && widget->isVisible())
                prompt = static_cast<QMessageBox *>(widget);
        }
        return prompt != nullptr;
    });
    return prompt;
}

void test_trust_flow(int mqtt, int ftps, int control_port, const char *fingerprint)
{
    struct bambu_profile_list profiles;
    const QString passed = QString::fromUtf8(obs_module_text("BambuOverlay.Settings.TestPassed"));
    const QString declined = QString::fromUtf8(obs_module_text("BambuOverlay.Settings.NotTrusted"));
    const QString refused = QString::fromUtf8(obs_module_text("BambuOverlay.Settings.AuthFailed"));

    bambu_settings_set_ports(mqtt, ftps);
    bambu_settings_open();
    QDialog *dialog = wait_for_dialog();
    CHECK(dialog != nullptr);
    if (dialog == nullptr)
        return;

    click(dialog, "addProfile");
    set_field(dialog, "host", "127.0.0.1");
    set_field(dialog, "serial", "TESTSERIAL0001");
    set_field(dialog, "accessCode", "12345678");

    /* Declining pins nothing, and the access code never leaves the plugin. */
    click(dialog, "test");
    QMessageBox *prompt = wait_for_trust_prompt();
    CHECK(prompt != nullptr);
    if (prompt == nullptr)
        return;
    CHECK(prompt->text().contains(QString::fromUtf8(fingerprint), Qt::CaseInsensitive));
    CHECK(prompt->defaultButton() == prompt->button(QMessageBox::No));
    CHECK(printer_logins(control_port) == 0);
    prompt->button(QMessageBox::No)->click();
    CHECK(wait_for([dialog, &declined]() { return status_text(dialog) == declined; }));
    CHECK(enabled(dialog, "test"));
    bambu_profile_store_list(&profiles);
    CHECK(profiles.count == 1);
    CHECK_STR(profiles.profiles[0].mqtt_fingerprint, "");
    CHECK(printer_logins(control_port) == 0);

    /* Accepting pins the certificate, and only then is the code sent. */
    click(dialog, "test");
    prompt = wait_for_trust_prompt();
    CHECK(prompt != nullptr);
    if (prompt == nullptr)
        return;
    prompt->button(QMessageBox::Yes)->click();
    CHECK(wait_for([dialog, &passed]() { return status_text(dialog) == passed; }));
    bambu_profile_store_list(&profiles);
    CHECK(_stricmp(profiles.profiles[0].mqtt_fingerprint, fingerprint) == 0);
    CHECK(printer_logins(control_port) == 1);

    /* A pinned, unchanged certificate does not ask again, and a bad code is refused. */
    set_field(dialog, "accessCode", "00000000");
    click(dialog, "test");
    CHECK(wait_for([dialog, &refused]() { return status_text(dialog) == refused; }));
    CHECK(printer_logins(control_port) == 2);

    click(dialog, "removeProfile");
    click(dialog, "close");
    CHECK(wait_for([]() { return open_dialogs().isEmpty(); }));
}

} // namespace

extern "C" const char *obs_module_text(const char *key)
{
    for (int i = 0; i < locale_count; i++) {
        if (strcmp(locale_entries[i].key, key) == 0)
            return locale_entries[i].value;
    }
    fprintf(stderr, "locale key missing from en-US.ini: %s\n", key);
    bambu_test_failures++;
    return key;
}

/* Stands in for the lookup OBS_MODULE_USE_DEFAULT_LOCALE provides in the plugin. */
extern "C" obs_module_t *obs_current_module(void)
{
    return nullptr;
}

extern "C" void bambu_overlay_refresh_properties(void)
{
    refresh_count++;
}

int main(int argc, char **argv)
{
    const char *temp = getenv("BAMBU_DIALOG_TEMP");
    char profile_path[1024];

    if (temp == nullptr || getenv("BAMBU_DIALOG_LOCALE") == nullptr) {
        fputs("BAMBU_DIALOG_LOCALE and BAMBU_DIALOG_TEMP must be set\n", stderr);
        return 2;
    }

    QApplication application(argc, argv);
    WSADATA winsock;

    WSAStartup(MAKEWORD(2, 2), &winsock);
    load_locale(getenv("BAMBU_DIALOG_LOCALE"));
    snprintf(profile_path, sizeof(profile_path), "%s/dialog-test-profiles-%d.json", temp, argc);
    bambu_delete_file(profile_path);
    bambu_profile_store_init(profile_path);
    bambu_services_init(load_config);

    if (argc >= 6) {
        test_trust_flow(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), argv[4]);
    } else {
        test_opens_once_on_the_ui_thread();
        test_form();
        test_close_cancels_a_running_test();
        test_shutdown_with_dialog_open();
    }

    bambu_settings_shutdown();
    bambu_services_shutdown();
    bambu_profile_store_shutdown();
    bambu_delete_file(profile_path);

    if (bambu_test_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", bambu_test_failures);
        return 1;
    }
    puts("settings dialog tests passed");
    return 0;
}
