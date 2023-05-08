// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <functional>
#include <iosfwd>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <QBoxLayout>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QStringLiteral>
#include <QtCore/qobjectdefs.h>
#include <qcoreevent.h>
#include <qglobal.h>
#include <vulkan/vulkan_core.h>

#include "common/common_types.h"
#include "common/dynamic_library.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "ui_configure_graphics.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_graphics.h"
#include "yuzu/qt_common.h"
#include "yuzu/uisettings.h"
#include "yuzu/vk_device_info.h"

static const std::vector<VkPresentModeKHR> default_present_modes{VK_PRESENT_MODE_IMMEDIATE_KHR,
                                                                 VK_PRESENT_MODE_FIFO_KHR};

// Converts a setting to a present mode (or vice versa)
static constexpr VkPresentModeKHR VSyncSettingToMode(Settings::VSyncMode mode) {
    switch (mode) {
    case Settings::VSyncMode::Immediate:
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case Settings::VSyncMode::Mailbox:
        return VK_PRESENT_MODE_MAILBOX_KHR;
    case Settings::VSyncMode::FIFO:
        return VK_PRESENT_MODE_FIFO_KHR;
    case Settings::VSyncMode::FIFORelaxed:
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    default:
        return VK_PRESENT_MODE_FIFO_KHR;
    }
}

static constexpr Settings::VSyncMode PresentModeToSetting(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return Settings::VSyncMode::Immediate;
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return Settings::VSyncMode::Mailbox;
    case VK_PRESENT_MODE_FIFO_KHR:
        return Settings::VSyncMode::FIFO;
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return Settings::VSyncMode::FIFORelaxed;
    default:
        return Settings::VSyncMode::FIFO;
    }
}

ConfigureGraphics::ConfigureGraphics(
    const Core::System& system_, std::vector<VkDeviceInfo::Record>& records_,
    const std::function<void()>& expose_compute_option_,
    std::shared_ptr<std::forward_list<ConfigurationShared::Tab*>> group,
    const ConfigurationShared::TranslationMap& translations_, QWidget* parent)
    : ConfigurationShared::Tab(group, parent), ui{std::make_unique<Ui::ConfigureGraphics>()},
      records{records_}, expose_compute_option{expose_compute_option_}, system{system_},
      translations{translations_} {
    vulkan_device = Settings::values.vulkan_device.GetValue();
    RetrieveVulkanDevices();

    ui->setupUi(this);

    SetConfiguration();

    for (const auto& device : vulkan_devices) {
        vulkan_device_combobox->addItem(device);
    }

    UpdateBackgroundColorButton(QColor::fromRgb(Settings::values.bg_red.GetValue(),
                                                Settings::values.bg_green.GetValue(),
                                                Settings::values.bg_blue.GetValue()));
    UpdateAPILayout();
    PopulateVSyncModeSelection(); //< must happen after UpdateAPILayout
    // SetFSRIndicatorText(ui->fsr_sharpening_slider->sliderPosition());

    // VSync setting needs to be determined after populating the VSync combobox
    if (Settings::IsConfiguringGlobal()) {
        const auto vsync_mode_setting = Settings::values.vsync_mode.GetValue();
        const auto vsync_mode = VSyncSettingToMode(vsync_mode_setting);
        int index{};
        for (const auto mode : vsync_mode_combobox_enum_map) {
            if (mode == vsync_mode) {
                break;
            }
            index++;
        }
        if (static_cast<unsigned long>(index) < vsync_mode_combobox_enum_map.size()) {
            vsync_mode_combobox->setCurrentIndex(index);
        }
    }

    connect(api_combobox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        UpdateAPILayout();
        PopulateVSyncModeSelection();
    });
    connect(vulkan_device_combobox, qOverload<int>(&QComboBox::activated), this,
            [this](int device) {
                UpdateDeviceSelection(device);
                PopulateVSyncModeSelection();
            });
    connect(shader_backend_combobox, qOverload<int>(&QComboBox::activated), this,
            [this](int backend) { UpdateShaderBackendSelection(backend); });

    // connect(ui->bg_button, &QPushButton::clicked, this, [this] {
    //     const QColor new_bg_color = QColorDialog::getColor(bg_color);
    //     if (!new_bg_color.isValid()) {
    //         return;
    //     }
    //     UpdateBackgroundColorButton(new_bg_color);
    // });

    api_combobox->setEnabled(!UISettings::values.has_broken_vulkan && api_combobox->isEnabled());
    ui->api_widget->setEnabled(
        (!UISettings::values.has_broken_vulkan || Settings::IsConfiguringGlobal()) &&
        ui->api_widget->isEnabled());
    // ui->bg_label->setVisible(Settings::IsConfiguringGlobal());
    // ui->bg_combobox->setVisible(!Settings::IsConfiguringGlobal());

    // connect(ui->fsr_sharpening_slider, &QSlider::valueChanged, this,
    //         &ConfigureGraphics::SetFSRIndicatorText);
    // ui->fsr_sharpening_combobox->setVisible(!Settings::IsConfiguringGlobal());
    // ui->fsr_sharpening_label->setVisible(Settings::IsConfiguringGlobal());
}

void ConfigureGraphics::PopulateVSyncModeSelection() {
    const Settings::RendererBackend backend{GetCurrentGraphicsBackend()};
    if (backend == Settings::RendererBackend::Null) {
        vsync_mode_combobox->setEnabled(false);
        return;
    }
    vsync_mode_combobox->setEnabled(true);

    const int current_index = //< current selected vsync mode from combobox
        vsync_mode_combobox->currentIndex();
    const auto current_mode = //< current selected vsync mode as a VkPresentModeKHR
        current_index == -1 ? VSyncSettingToMode(Settings::values.vsync_mode.GetValue())
                            : vsync_mode_combobox_enum_map[current_index];
    int index{};
    const int device{vulkan_device_combobox->currentIndex()}; //< current selected Vulkan device
    const auto& present_modes = //< relevant vector of present modes for the selected device or API
        backend == Settings::RendererBackend::Vulkan ? device_present_modes[device]
                                                     : default_present_modes;

    vsync_mode_combobox->clear();
    vsync_mode_combobox_enum_map.clear();
    vsync_mode_combobox_enum_map.reserve(present_modes.size());
    for (const auto present_mode : present_modes) {
        const auto mode_name = TranslateVSyncMode(present_mode, backend);
        if (mode_name.isEmpty()) {
            continue;
        }

        vsync_mode_combobox->insertItem(index, mode_name);
        vsync_mode_combobox_enum_map.push_back(present_mode);
        if (present_mode == current_mode) {
            vsync_mode_combobox->setCurrentIndex(index);
        }
        index++;
    }
}

void ConfigureGraphics::UpdateDeviceSelection(int device) {
    if (device == -1) {
        return;
    }
    if (GetCurrentGraphicsBackend() == Settings::RendererBackend::Vulkan) {
        vulkan_device = device;
    }
}

void ConfigureGraphics::UpdateShaderBackendSelection(int backend) {
    if (backend == -1) {
        return;
    }
    if (GetCurrentGraphicsBackend() == Settings::RendererBackend::OpenGL) {
        shader_backend = static_cast<Settings::ShaderBackend>(backend);
    }
}

ConfigureGraphics::~ConfigureGraphics() = default;

void ConfigureGraphics::SetConfiguration() {
    const bool runtime_lock = !system.IsPoweredOn();
    QLayout& api_layout = *ui->api_widget->layout();
    QLayout& graphics_layout = *ui->graphics_widget->layout();

    std::map<bool, std::map<std::string, QWidget*>> hold_graphics;
    std::forward_list<QWidget*> hold_api;

    for (const auto setting : Settings::values.linkage.by_category[Settings::Category::Renderer]) {
        const auto& setting_label = setting->GetLabel();

        auto [widget, extra] = [&]() {
            if (setting->Id() == Settings::values.vulkan_device.Id() ||
                setting->Id() == Settings::values.shader_backend.Id() ||
                setting->Id() == Settings::values.vsync_mode.Id()) {
                return ConfigurationShared::CreateWidget(
                    setting, translations, this, runtime_lock, apply_funcs, trackers,
                    ConfigurationShared::RequestType::ComboBox, false);
            } else {
                return ConfigurationShared::CreateWidget(setting, translations, this, runtime_lock,
                                                         apply_funcs, trackers);
            }
        }();

        if (widget == nullptr) {
            continue;
        }

        if (setting->Id() == Settings::values.vulkan_device.Id()) {
            api_layout.addWidget(widget);
            api_combobox = reinterpret_cast<QComboBox*>(extra);
        } else if (setting->Id() == Settings::values.vulkan_device.Id()) {
            hold_api.push_front(widget);
            vulkan_device_combobox = reinterpret_cast<QComboBox*>(extra);
            vulkan_device_widget = widget;
        } else if (setting->Id() == Settings::values.shader_backend.Id()) {
            hold_api.push_front(widget);
            shader_backend_combobox = reinterpret_cast<QComboBox*>(extra);
            shader_backend_widget = widget;
        } else if (setting->Id() == Settings::values.vsync_mode.Id()) {
            vsync_mode_combobox = reinterpret_cast<QComboBox*>(extra);
            hold_graphics[setting->IsEnum()][setting_label] = widget;
        } else {
            hold_graphics[setting->IsEnum()][setting_label] = widget;
        }
    }

    for (const auto& [_, settings] : hold_graphics) {
        for (const auto& [label, widget] : settings) {
            graphics_layout.addWidget(widget);
        }
    }

    for (auto widget : hold_api) {
        api_layout.addWidget(widget);
    }
}

void ConfigureGraphics::SetFSRIndicatorText(int percentage) {
    // ui->fsr_sharpening_value->setText(
    //     tr("%1%", "FSR sharpening percentage (e.g. 50%)").arg(100 - (percentage / 2)));
}

const QString ConfigureGraphics::TranslateVSyncMode(VkPresentModeKHR mode,
                                                    Settings::RendererBackend backend) const {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return backend == Settings::RendererBackend::OpenGL
                   ? tr("Off")
                   : QStringLiteral("Immediate (%1)").arg(tr("VSync Off"));
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return QStringLiteral("Mailbox (%1)").arg(tr("Recommended"));
    case VK_PRESENT_MODE_FIFO_KHR:
        return backend == Settings::RendererBackend::OpenGL
                   ? tr("On")
                   : QStringLiteral("FIFO (%1)").arg(tr("VSync On"));
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return QStringLiteral("FIFO Relaxed");
    default:
        return {};
        break;
    }
}

void ConfigureGraphics::ApplyConfiguration() {
    const bool powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(powered_on);
    }

    if (Settings::IsConfiguringGlobal()) {
        const auto mode = vsync_mode_combobox_enum_map[vsync_mode_combobox->currentIndex()];
        const auto vsync_mode = PresentModeToSetting(mode);
        Settings::values.vsync_mode.SetValue(vsync_mode);
    }
}

void ConfigureGraphics::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureGraphics::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureGraphics::UpdateBackgroundColorButton(QColor color) {
    bg_color = color;

    // QPixmap pixmap(ui->bg_button->size());
    // pixmap.fill(bg_color);

    // const QIcon color_icon(pixmap);
    // ui->bg_button->setIcon(color_icon);
}

void ConfigureGraphics::UpdateAPILayout() {
    if (!Settings::IsConfiguringGlobal() &&
        api_combobox->currentIndex() == ConfigurationShared::USE_GLOBAL_INDEX) {
        vulkan_device = Settings::values.vulkan_device.GetValue(true);
        shader_backend = Settings::values.shader_backend.GetValue(true);
        vulkan_device_widget->setEnabled(false);
        shader_backend_widget->setEnabled(false);
    } else {
        vulkan_device = Settings::values.vulkan_device.GetValue();
        shader_backend = Settings::values.shader_backend.GetValue();
        vulkan_device_widget->setEnabled(true);
        shader_backend_widget->setEnabled(true);
    }

    switch (GetCurrentGraphicsBackend()) {
    case Settings::RendererBackend::OpenGL:
        shader_backend_combobox->setCurrentIndex(static_cast<u32>(shader_backend));
        vulkan_device_widget->setVisible(false);
        shader_backend_widget->setVisible(true);
        break;
    case Settings::RendererBackend::Vulkan:
        if (static_cast<int>(vulkan_device) < vulkan_device_combobox->count()) {
            vulkan_device_combobox->setCurrentIndex(vulkan_device);
        }
        vulkan_device_widget->setVisible(true);
        shader_backend_widget->setVisible(false);
        break;
    case Settings::RendererBackend::Null:
        vulkan_device_widget->setVisible(false);
        shader_backend_widget->setVisible(false);
        break;
    }
}

void ConfigureGraphics::RetrieveVulkanDevices() {
    vulkan_devices.clear();
    vulkan_devices.reserve(records.size());
    device_present_modes.clear();
    device_present_modes.reserve(records.size());
    for (const auto& record : records) {
        vulkan_devices.push_back(QString::fromStdString(record.name));
        device_present_modes.push_back(record.vsync_support);

        if (record.has_broken_compute) {
            expose_compute_option();
        }
    }
}

Settings::RendererBackend ConfigureGraphics::GetCurrentGraphicsBackend() const {
    if (Settings::IsConfiguringGlobal()) {
        return static_cast<Settings::RendererBackend>(api_combobox->currentIndex());
    }

    if (api_combobox->currentIndex() == ConfigurationShared::USE_GLOBAL_INDEX) {
        Settings::values.renderer_backend.SetGlobal(true);
        return Settings::values.renderer_backend.GetValue();
    }
    Settings::values.renderer_backend.SetGlobal(false);
    return static_cast<Settings::RendererBackend>(api_combobox->currentIndex() -
                                                  ConfigurationShared::USE_GLOBAL_OFFSET);
}
