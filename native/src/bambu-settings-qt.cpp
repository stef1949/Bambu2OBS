/* The Tools-menu connection settings. It is a Qt dialog so that it takes OBS's own
 * theme; Qt comes with OBS, so the plugin still ships nothing beside its DLL.
 *
 * Neither the dialog nor the trust prompt runs a nested event loop. Plugin code on
 * the stack of an event loop would still be there when OBS unloads the module. */

#include "bambu-settings.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

extern "C" {
#include "printer-service.h"
}

namespace {

class SettingsDialog;

SettingsDialog *current_dialog = nullptr;
int mqtt_port = BAMBU_DEFAULT_MQTT_PORT;
int ftps_port = BAMBU_DEFAULT_FTPS_PORT;

QString localized(const char *key)
{
    return QString::fromUtf8(obs_module_text(key));
}

/* Pasted hosts and codes often carry stray whitespace. */
void read_field(const QLineEdit *field, char *out, size_t capacity)
{
    QByteArray text = field->text().trimmed().toUtf8();

    bambu_copy_text(out, capacity, text.constData());
    SecureZeroMemory(text.data(), static_cast<size_t>(text.size()));
}

struct ProbeJob {
    char host[BAMBU_HOST_CAPACITY] = {};
    char pinned[BAMBU_FINGERPRINT_CAPACITY] = {};
    char access_code[BAMBU_ACCESS_CODE_CAPACITY] = {};
    struct bambu_cancel *cancel = bambu_cancel_create();
    struct bambu_certificate_probe certificates = {};
    enum bambu_net_result login = BAMBU_NET_OK;

    ProbeJob() = default;
    ProbeJob(const ProbeJob &) = delete;
    ProbeJob &operator=(const ProbeJob &) = delete;

    ~ProbeJob()
    {
        SecureZeroMemory(access_code, sizeof(access_code));
        bambu_cancel_destroy(cancel);
    }
};

class SettingsDialog : public QDialog {
public:
    explicit SettingsDialog(QWidget *parent) : QDialog(parent)
    {
        current_dialog = this;
        setObjectName("bambuSettings");
        setWindowTitle(localized("BambuOverlay.Settings.Title"));
        setMinimumSize(780, 560);
        build_layout();
        connect_controls();

        /* Sources list printers in their properties, so a change must reach them. */
        connect(this, &QDialog::finished, this, [this]() {
            bambu_overlay_refresh_properties();
            deleteLater();
        });

        bambu_profile_store_list(&profiles);
        selected = profiles.count > 0 ? 0 : -1;
        fill_list();
        load_form();
    }

    ~SettingsDialog() override
    {
        finish_worker();
        if (current_dialog == this)
            current_dialog = nullptr;
    }

    void done(int result) override
    {
        finish_worker();
        QDialog::done(result);
    }

private:
    struct bambu_profile_list profiles = {};
    int selected = -1;
    std::thread worker;
    std::unique_ptr<ProbeJob> job;

    QListWidget *profile_list = nullptr;
    QPushButton *add_button = nullptr;
    QPushButton *remove_button = nullptr;
    QLineEdit *name = nullptr;
    QLineEdit *host = nullptr;
    QLineEdit *serial = nullptr;
    QLineEdit *access_code = nullptr;
    QCheckBox *preview_enabled = nullptr;
    QCheckBox *lan_only = nullptr;
    QLineEdit *cloud_token = nullptr;
    QCheckBox *china_region = nullptr;
    QLabel *fingerprints = nullptr;
    QLabel *status = nullptr;
    QPushButton *save_button = nullptr;
    QPushButton *test_button = nullptr;
    QPushButton *reconnect_button = nullptr;
    QPushButton *clear_preview_button = nullptr;
    QPushButton *export_button = nullptr;
    QPushButton *close_button = nullptr;

    template<typename T, typename... Args> T *named(const char *object_name, Args &&...args)
    {
        T *widget = new T(std::forward<Args>(args)...);
        widget->setObjectName(object_name);
        return widget;
    }

    QPushButton *button(const char *object_name, const char *text_key)
    {
        QPushButton *result = named<QPushButton>(object_name, localized(text_key));
        /* Enter saves; only the Save button is the default. */
        result->setAutoDefault(false);
        return result;
    }

    QLineEdit *secret_field(const char *object_name, int capacity)
    {
        QLineEdit *field = named<QLineEdit>(object_name);
        field->setEchoMode(QLineEdit::Password);
        field->setMaxLength(capacity - 1);
        field->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhSensitiveData | Qt::ImhNoPredictiveText);
        return field;
    }

    void build_layout()
    {
        profile_list = named<QListWidget>("profileList");
        add_button = button("addProfile", "BambuOverlay.Settings.Add");
        remove_button = button("removeProfile", "BambuOverlay.Settings.Remove");

        QHBoxLayout *list_buttons = new QHBoxLayout;
        list_buttons->addWidget(add_button);
        list_buttons->addWidget(remove_button);

        QVBoxLayout *left = new QVBoxLayout;
        left->addWidget(profile_list, 1);
        left->addLayout(list_buttons);

        name = named<QLineEdit>("name");
        host = named<QLineEdit>("host");
        serial = named<QLineEdit>("serial");
        access_code = secret_field("accessCode", BAMBU_ACCESS_CODE_CAPACITY);
        preview_enabled = named<QCheckBox>("previewEnabled", localized("BambuOverlay.Settings.Preview"));
        lan_only = named<QCheckBox>("lanOnly", localized("BambuOverlay.Settings.LanOnly"));
        cloud_token = secret_field("cloudToken", BAMBU_CLOUD_TOKEN_CAPACITY);
        china_region = named<QCheckBox>("chinaRegion", localized("BambuOverlay.Settings.ChinaRegion"));

        QFormLayout *form = new QFormLayout;
        form->addRow(localized("BambuOverlay.Settings.Name"), name);
        form->addRow(localized("BambuOverlay.Settings.Host"), host);
        form->addRow(localized("BambuOverlay.Settings.Serial"), serial);
        form->addRow(localized("BambuOverlay.Settings.AccessCode"), access_code);
        form->addRow(QString(), preview_enabled);
        form->addRow(QString(), lan_only);
        form->addRow(localized("BambuOverlay.Settings.CloudToken"), cloud_token);
        form->addRow(QString(), china_region);

        QLabel *fingerprint_title = new QLabel(localized("BambuOverlay.Settings.Fingerprint"));
        fingerprints = named<QLabel>("fingerprints");
        fingerprints->setTextFormat(Qt::PlainText);
        fingerprints->setTextInteractionFlags(Qt::TextSelectableByMouse);

        status = named<QLabel>("status");
        status->setTextFormat(Qt::PlainText);
        status->setTextInteractionFlags(Qt::TextSelectableByMouse);
        status->setWordWrap(true);
        status->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        status->setMinimumHeight(status->fontMetrics().lineSpacing() * 3);

        save_button = button("save", "BambuOverlay.Settings.Save");
        save_button->setDefault(true);
        test_button = button("test", "BambuOverlay.Settings.Test");
        reconnect_button = button("reconnect", "BambuOverlay.Reconnect");
        clear_preview_button = button("clearPreview", "BambuOverlay.Settings.ClearPreview");

        QHBoxLayout *actions = new QHBoxLayout;
        actions->addWidget(save_button);
        actions->addWidget(test_button);
        actions->addWidget(reconnect_button);
        actions->addWidget(clear_preview_button);

        QVBoxLayout *right = new QVBoxLayout;
        right->addLayout(form);
        right->addWidget(fingerprint_title);
        right->addWidget(fingerprints);
        right->addWidget(status, 1);
        right->addLayout(actions);

        QHBoxLayout *columns = new QHBoxLayout;
        columns->addLayout(left, 1);
        columns->addLayout(right, 3);

        QLabel *disclaimer = named<QLabel>("disclaimer", localized("BambuOverlay.Disclaimer"));
        disclaimer->setTextFormat(Qt::PlainText);
        disclaimer->setWordWrap(true);
        disclaimer->setProperty("class", "text-small");
        export_button = button("export", "BambuOverlay.ExportDiagnostics");
        close_button = button("close", "BambuOverlay.Settings.Close");

        QHBoxLayout *footer = new QHBoxLayout;
        footer->addWidget(disclaimer, 1);
        footer->addWidget(export_button);
        footer->addWidget(close_button);

        QVBoxLayout *root = new QVBoxLayout(this);
        root->addLayout(columns, 1);
        root->addLayout(footer);
    }

    void connect_controls()
    {
        /* Not currentRowChanged: assistive tools can select a row without making it
         * current, which would leave the form editing a different profile. */
        connect(profile_list, &QListWidget::itemSelectionChanged, this, [this]() {
            for (QListWidgetItem *item : profile_list->selectedItems()) {
                const int row = profile_list->row(item);
                if (row == selected)
                    continue;
                selected = row;
                /* Making it current also leaves it as the only selected row. */
                const QSignalBlocker blocker(profile_list);
                profile_list->setCurrentRow(row);
                load_form();
                return;
            }
        });
        connect(add_button, &QPushButton::clicked, this, [this]() { add_profile(); });
        connect(remove_button, &QPushButton::clicked, this, [this]() { remove_profile(); });
        connect(save_button, &QPushButton::clicked, this, [this]() {
            set_status(save_form() ? "BambuOverlay.Settings.Saved" : "BambuOverlay.Settings.SaveFailed");
        });
        connect(test_button, &QPushButton::clicked, this, [this]() { start_test(); });
        connect(reconnect_button, &QPushButton::clicked, this, [this]() { with_service(bambu_service_reconnect); });
        connect(clear_preview_button, &QPushButton::clicked, this, [this]() {
            with_service(bambu_service_clear_preview);
            set_status("BambuOverlay.Settings.PreviewCleared");
        });
        connect(export_button, &QPushButton::clicked, this, []() { bambu_settings_export_diagnostics(); });
        connect(close_button, &QPushButton::clicked, this, &QDialog::reject);
    }

    struct bambu_profile *selected_profile() { return &profiles.profiles[selected]; }

    void set_status(const char *text_key) { status->setText(localized(text_key)); }

    void set_status_with_reason(const char *text_key, enum bambu_net_result result)
    {
        status->setText(QString("%1: %2").arg(localized(text_key), QString::fromUtf8(bambu_net_result_name(result))));
    }

    void fill_list()
    {
        const QSignalBlocker blocker(profile_list);

        profile_list->clear();
        for (int i = 0; i < profiles.count; i++) {
            const struct bambu_profile &profile = profiles.profiles[i];
            profile_list->addItem(QString::fromUtf8(profile.name[0] != '\0' ? profile.name : profile.id));
        }
        profile_list->setCurrentRow(selected);
    }

    void show_fingerprints(const struct bambu_profile *profile)
    {
        char text[128];

        /* Abbreviated to fit; the trust prompt always shows the whole value. */
        snprintf(
            text,
            sizeof(text),
            "MQTT: %.32s%s\nFTPS: %.32s%s",
            profile->mqtt_fingerprint[0] != '\0' ? profile->mqtt_fingerprint : "-",
            profile->mqtt_fingerprint[0] != '\0' ? "..." : "",
            profile->ftps_fingerprint[0] != '\0' ? profile->ftps_fingerprint : "-",
            profile->ftps_fingerprint[0] != '\0' ? "..." : ""
        );
        fingerprints->setText(QString::fromUtf8(text));
    }

    void set_form_enabled(bool enabled)
    {
        QWidget *const controls[] = {
            name,        host,        serial,           access_code,          preview_enabled,
            lan_only,    cloud_token, china_region,     save_button,          test_button,
            remove_button, reconnect_button, clear_preview_button,
        };
        for (QWidget *control : controls)
            control->setEnabled(enabled);
    }

    /* A test's result applies to the selected profile, so that must not change
     * underneath it, or one printer's certificate could be pinned to another. */
    void set_testing(bool testing)
    {
        QWidget *const controls[] = {profile_list, add_button, remove_button, save_button, test_button};
        for (QWidget *control : controls)
            control->setEnabled(!testing);
    }

    void load_form()
    {
        set_form_enabled(selected >= 0);
        /* Stored secrets are never read back into the dialog; a blank field keeps them. */
        access_code->clear();
        cloud_token->clear();
        if (selected < 0) {
            name->clear();
            host->clear();
            serial->clear();
            fingerprints->clear();
            status->clear();
            return;
        }

        const struct bambu_profile *profile = selected_profile();
        name->setText(QString::fromUtf8(profile->name));
        host->setText(QString::fromUtf8(profile->host));
        serial->setText(QString::fromUtf8(profile->serial));
        lan_only->setChecked(profile->lan_only);
        china_region->setChecked(profile->cloud_china_region);
        preview_enabled->setChecked(profile->preview_enabled);
        show_fingerprints(profile);
        set_status(
            bambu_credential_exists(profile->id, BAMBU_SECRET_ACCESS_CODE) ? "BambuOverlay.Settings.CodeStored"
                                                                           : "BambuOverlay.Settings.CodeMissing"
        );
    }

    /* Writes the form into the selected profile and persists it. */
    bool save_form()
    {
        char secret[BAMBU_CLOUD_TOKEN_CAPACITY];
        char previous_host[BAMBU_HOST_CAPACITY];

        if (selected < 0)
            return false;
        struct bambu_profile *profile = selected_profile();
        memcpy(previous_host, profile->host, sizeof(previous_host));

        read_field(name, profile->name, sizeof(profile->name));
        read_field(host, profile->host, sizeof(profile->host));
        read_field(serial, profile->serial, sizeof(profile->serial));
        profile->lan_only = lan_only->isChecked();
        profile->cloud_china_region = china_region->isChecked();
        profile->preview_enabled = preview_enabled->isChecked();
        if (profile->name[0] == '\0')
            bambu_copy_text(profile->name, sizeof(profile->name), profile->host);

        /* A pin belongs to one device, so a new address must be trusted afresh. */
        if (strcmp(previous_host, profile->host) != 0) {
            profile->mqtt_fingerprint[0] = '\0';
            profile->ftps_fingerprint[0] = '\0';
        }

        bool ok = bambu_profile_store_put(profile);

        read_field(access_code, secret, BAMBU_ACCESS_CODE_CAPACITY);
        if (secret[0] != '\0')
            ok = bambu_credential_write(profile->id, BAMBU_SECRET_ACCESS_CODE, secret) && ok;
        read_field(cloud_token, secret, sizeof(secret));
        if (secret[0] != '\0')
            ok = bambu_credential_write(profile->id, BAMBU_SECRET_CLOUD_TOKEN, secret) && ok;
        SecureZeroMemory(secret, sizeof(secret));

        access_code->clear();
        cloud_token->clear();
        show_fingerprints(profile);
        fill_list();
        bambu_services_profile_changed(profile->id);
        return ok;
    }

    void finish_worker()
    {
        if (worker.joinable()) {
            if (job != nullptr)
                bambu_cancel_request(job->cancel);
            worker.join();
        }
        job.reset();
    }

    std::unique_ptr<ProbeJob> new_job(const struct bambu_profile *profile)
    {
        std::unique_ptr<ProbeJob> result = std::make_unique<ProbeJob>();
        bambu_copy_text(result->host, sizeof(result->host), profile->host);
        return result;
    }

    void start_test()
    {
        if (worker.joinable() || !save_form())
            return;

        const struct bambu_profile *profile = selected_profile();
        if (profile->host[0] == '\0') {
            set_status("BambuOverlay.Settings.HostRequired");
            return;
        }
        set_status("BambuOverlay.Settings.Testing");
        set_testing(true);
        job = new_job(profile);
        worker = std::thread([this, active = job.get()]() {
            bambu_probe_certificates(active->host, mqtt_port, ftps_port, active->cancel, &active->certificates);
            QMetaObject::invokeMethod(this, [this]() { on_certificates(); }, Qt::QueuedConnection);
        });
    }

    /* The certificate is shown to the user before any credential is sent to it, so
     * a rogue device on the network cannot harvest the access code during a test. */
    void on_certificates()
    {
        if (job == nullptr)
            return;

        const struct bambu_certificate_probe probe = job->certificates;
        const struct bambu_profile *profile = selected_profile();

        finish_worker();

        if (probe.mqtt_result != BAMBU_NET_OK) {
            set_status_with_reason("BambuOverlay.Settings.Unreachable", probe.mqtt_result);
            set_testing(false);
            return;
        }

        const bool changed = strcmp(profile->mqtt_fingerprint, probe.mqtt_fingerprint) != 0 ||
                             (probe.ftps_result == BAMBU_NET_OK &&
                              strcmp(profile->ftps_fingerprint, probe.ftps_fingerprint) != 0);
        if (changed)
            ask_to_trust(probe);
        else
            start_login();
    }

    void ask_to_trust(const struct bambu_certificate_probe &probe)
    {
        const struct bambu_profile *profile = selected_profile();
        const QString message =
            QString("%1\n\nMQTT SHA-256:\n%2\n\nFTPS SHA-256:\n%3")
                .arg(localized(
                         profile->mqtt_fingerprint[0] != '\0' ? "BambuOverlay.Settings.CertChanged"
                                                              : "BambuOverlay.Settings.TrustPrompt"
                     ),
                     QString::fromUtf8(probe.mqtt_fingerprint),
                     probe.ftps_result == BAMBU_NET_OK ? QString::fromUtf8(probe.ftps_fingerprint) : QString("-"));

        QMessageBox *prompt = new QMessageBox(
            QMessageBox::Question,
            localized("BambuOverlay.Settings.TrustTitle"),
            message,
            QMessageBox::Yes | QMessageBox::No,
            this
        );
        prompt->setObjectName("trustPrompt");
        prompt->setTextFormat(Qt::PlainText);
        prompt->setTextInteractionFlags(Qt::TextSelectableByMouse);
        prompt->setDefaultButton(QMessageBox::No);
        prompt->setEscapeButton(QMessageBox::No);
        prompt->setAttribute(Qt::WA_DeleteOnClose);
        connect(prompt, &QDialog::finished, this, [this, prompt, probe]() {
            if (prompt->clickedButton() == prompt->button(QMessageBox::Yes))
                accept_certificates(probe);
            else
                decline_certificates();
        });
        prompt->open();
    }

    void decline_certificates()
    {
        set_status("BambuOverlay.Settings.NotTrusted");
        set_testing(false);
    }

    void accept_certificates(const struct bambu_certificate_probe &probe)
    {
        struct bambu_profile *profile = selected_profile();

        memcpy(profile->mqtt_fingerprint, probe.mqtt_fingerprint, sizeof(profile->mqtt_fingerprint));
        if (probe.ftps_result == BAMBU_NET_OK)
            memcpy(profile->ftps_fingerprint, probe.ftps_fingerprint, sizeof(profile->ftps_fingerprint));
        bambu_profile_store_put(profile);
        show_fingerprints(profile);
        start_login();
    }

    void start_login()
    {
        const struct bambu_profile *profile = selected_profile();
        char code[BAMBU_ACCESS_CODE_CAPACITY];

        if (!bambu_credential_read(profile->id, BAMBU_SECRET_ACCESS_CODE, code, sizeof(code))) {
            set_status("BambuOverlay.Settings.CodeMissing");
            set_testing(false);
            bambu_services_profile_changed(profile->id);
            return;
        }
        job = new_job(profile);
        memcpy(job->pinned, profile->mqtt_fingerprint, sizeof(job->pinned));
        memcpy(job->access_code, code, sizeof(job->access_code));
        SecureZeroMemory(code, sizeof(code));
        worker = std::thread([this, active = job.get()]() {
            active->login = bambu_probe_login(active->host, mqtt_port, active->pinned, active->access_code, active->cancel);
            QMetaObject::invokeMethod(this, [this]() { on_login(); }, Qt::QueuedConnection);
        });
    }

    void on_login()
    {
        if (job == nullptr)
            return;

        const enum bambu_net_result result = job->login;
        const struct bambu_profile *profile = selected_profile();

        finish_worker();
        set_testing(false);
        if (result == BAMBU_NET_OK)
            set_status("BambuOverlay.Settings.TestPassed");
        else if (result == BAMBU_NET_AUTH_FAILED)
            set_status("BambuOverlay.Settings.AuthFailed");
        else
            set_status_with_reason("BambuOverlay.Settings.TestFailed", result);
        bambu_services_profile_changed(profile->id);
    }

    void add_profile()
    {
        if (profiles.count >= BAMBU_MAX_PROFILES)
            return;

        struct bambu_profile *profile = &profiles.profiles[profiles.count];
        bambu_profile_defaults(profile);
        bambu_profile_new_id(profile->id);
        bambu_copy_text(profile->name, sizeof(profile->name), obs_module_text("BambuOverlay.Settings.NewProfile"));
        selected = profiles.count++;
        bambu_profile_store_put(profile);
        fill_list();
        load_form();
    }

    void remove_profile()
    {
        char id[BAMBU_PROFILE_ID_CAPACITY];

        if (selected < 0)
            return;
        memcpy(id, selected_profile()->id, sizeof(id));
        bambu_credential_delete(id, BAMBU_SECRET_ACCESS_CODE);
        bambu_credential_delete(id, BAMBU_SECRET_CLOUD_TOKEN);
        bambu_profile_store_remove(id);
        bambu_services_profile_changed(id);
        bambu_profile_store_list(&profiles);
        selected = profiles.count > 0 ? 0 : -1;
        fill_list();
        load_form();
    }

    /* The service only exists while a source uses the profile. */
    void with_service(void (*action)(struct bambu_service *))
    {
        if (selected < 0)
            return;

        struct bambu_service *service = bambu_service_acquire(selected_profile()->id);
        if (service != nullptr) {
            action(service);
            bambu_service_release(service);
        }
    }
};

/* Reachable from Tools and from every source's properties; one at a time. */
void open_on_ui_thread()
{
    if (current_dialog != nullptr) {
        if (current_dialog->isVisible()) {
            current_dialog->raise();
            current_dialog->activateWindow();
            return;
        }
        /* Closed, and waiting for its deferred delete. */
        delete current_dialog;
    }

    /* A source's properties may be modal, and a dialog behind it could not be used. */
    QWidget *parent = QApplication::activeModalWidget();
    if (parent == nullptr)
        parent = static_cast<QWidget *>(obs_frontend_get_main_window());
    (new SettingsDialog(parent))->open();
}

void on_tools_menu(void *data)
{
    UNUSED_PARAMETER(data);
    bambu_settings_open();
}

} // namespace

extern "C" void bambu_settings_open(void)
{
    QCoreApplication *application = QCoreApplication::instance();

    if (application == nullptr)
        return;
    /* obs-websocket can press the source's button from one of its own threads. */
    if (QThread::currentThread() == application->thread())
        open_on_ui_thread();
    else
        QMetaObject::invokeMethod(application, open_on_ui_thread, Qt::QueuedConnection);
}

extern "C" void bambu_settings_shutdown(void)
{
    delete current_dialog;
}

extern "C" void bambu_settings_set_ports(int mqtt, int ftps)
{
    mqtt_port = mqtt;
    ftps_port = ftps;
}

extern "C" void bambu_settings_export_diagnostics(void)
{
    char *path = obs_module_config_path("diagnostics.txt");
    char *report = static_cast<char *>(bzalloc(64 * 1024));
    size_t used = static_cast<size_t>(snprintf(
        report,
        64 * 1024,
        "Bambu Printer Overlay diagnostics (redacted)\nplugin: %s\nlibobs: %s\n",
        BAMBU_OVERLAY_VERSION,
        obs_get_version_string()
    ));

    used += bambu_services_diagnostics(report + used, 64 * 1024 - used);

    if (path != nullptr && bambu_write_file_atomic(path, report, used)) {
        wchar_t wide[1100];
        if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1100) != 0)
            ShellExecuteW(nullptr, L"open", wide, nullptr, nullptr, SW_SHOWNORMAL);
    }
    bfree(report);
    bfree(path);
}

extern "C" void bambu_settings_register(void)
{
    obs_frontend_add_tools_menu_item(obs_module_text("BambuOverlay.Settings.Title"), on_tools_menu, nullptr);
}
