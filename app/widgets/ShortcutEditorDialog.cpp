#include "widgets/ShortcutEditorDialog.h"

#include <QtCore/QHash>
#include <QtCore/QSettings>
#include <QtGui/QAction>
#include <QtGui/QKeySequence>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QKeySequenceEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>

namespace vidscope::widgets {
namespace {

constexpr auto kDefaultsProperty = "vidscopeDefaultShortcuts";
constexpr auto kSettingsGroup = "shortcuts";
constexpr int kMaximumBindingColumns = 4;

[[nodiscard]] QStringList toPortableStrings(const QList<QKeySequence>& shortcuts)
{
    QStringList values;
    values.reserve(shortcuts.size());
    for (const auto& shortcut : shortcuts) {
        if (!shortcut.isEmpty()) {
            values.push_back(shortcut.toString(QKeySequence::PortableText));
        }
    }
    return values;
}

[[nodiscard]] QList<QKeySequence> fromPortableStrings(const QStringList& values)
{
    QList<QKeySequence> shortcuts;
    shortcuts.reserve(values.size());
    for (const auto& value : values) {
        const QKeySequence shortcut = QKeySequence::fromString(value, QKeySequence::PortableText);
        if (!shortcut.isEmpty()) {
            shortcuts.push_back(shortcut);
        }
    }
    return shortcuts;
}

[[nodiscard]] QString displayActionText(QAction* action)
{
    QString text = action ? action->text() : QString{};
    text.remove(QLatin1Char('&'));
    return text;
}

} // namespace

ShortcutEditorDialog::ShortcutEditorDialog(const QList<QAction*>& actions, QWidget* parent)
    : QDialog(parent)
{
    for (auto* action : actions) {
        if (action != nullptr && !action->objectName().isEmpty()
            && action->property(kDefaultsProperty).isValid()) {
            actions_.push_back(action);
        }
    }

    bindingColumnCount_ = 1;
    for (auto* action : actions_) {
        const int shortcutCount = static_cast<int>(std::min(
            action->shortcuts().size(),
            static_cast<qsizetype>(kMaximumBindingColumns)));
        const int defaultCount = static_cast<int>(std::min(
            action->property(kDefaultsProperty).toStringList().size(),
            static_cast<qsizetype>(kMaximumBindingColumns)));
        bindingColumnCount_ = std::max(
            bindingColumnCount_,
            std::max(shortcutCount, defaultCount));
    }

    setWindowTitle(tr("Keyboard Shortcuts"));
    setModal(true);
    resize(760, 480);

    auto* layout = new QVBoxLayout(this);
    table_ = new QTableWidget(actions_.size(), bindingColumnCount_ + 1, this);
    table_->setObjectName(QStringLiteral("shortcutTable"));
    table_->setAlternatingRowColors(true);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);

    QStringList headers{tr("Action")};
    for (int binding = 0; binding < bindingColumnCount_; ++binding) {
        headers.push_back(tr("Binding %1").arg(binding + 1));
    }
    table_->setHorizontalHeaderLabels(headers);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    for (int column = 1; column < table_->columnCount(); ++column) {
        table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Stretch);
    }
    table_->verticalHeader()->setVisible(false);

    for (int row = 0; row < actions_.size(); ++row) {
        const QString actionText = displayActionText(actions_.at(row));
        auto* label = new QTableWidgetItem(actionText);
        label->setFlags(label->flags() & ~Qt::ItemIsEditable);
        table_->setItem(row, 0, label);
        for (int binding = 0; binding < bindingColumnCount_; ++binding) {
            auto* editor = new QKeySequenceEdit(table_);
            editor->setClearButtonEnabled(true);
            editor->setAccessibleName(
                tr("%1, binding %2")
                    .arg(actionText)
                    .arg(binding + 1));
            table_->setCellWidget(row, binding + 1, editor);
        }
        setRowShortcuts(row, toPortableStrings(actions_.at(row)->shortcuts()));
    }
    layout->addWidget(table_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults,
        this);
    connect(buttons, &QDialogButtonBox::accepted, this, &ShortcutEditorDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(
        buttons->button(QDialogButtonBox::RestoreDefaults),
        &QPushButton::clicked,
        this,
        &ShortcutEditorDialog::restoreDefaults);
    layout->addWidget(buttons);
}

void ShortcutEditorDialog::configureAction(
    QAction* action,
    const QList<QKeySequence>& defaultShortcuts)
{
    if (action == nullptr || action->objectName().isEmpty()) {
        return;
    }

    const QStringList defaults = toPortableStrings(defaultShortcuts);
    action->setProperty(kDefaultsProperty, defaults);

    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kSettingsGroup));
    if (settings.contains(action->objectName())) {
        action->setShortcuts(fromPortableStrings(settings.value(action->objectName()).toStringList()));
    } else {
        action->setShortcuts(defaultShortcuts);
    }
    settings.endGroup();
}

void ShortcutEditorDialog::accept()
{
    QHash<QString, QString> owners;
    QList<QStringList> bindings;
    bindings.reserve(actions_.size());

    for (int row = 0; row < actions_.size(); ++row) {
        const QStringList shortcuts = shortcutsForRow(row);
        for (const auto& shortcut : shortcuts) {
            const auto existing = owners.constFind(shortcut);
            if (existing != owners.cend()) {
                QMessageBox::warning(
                    this,
                    tr("Shortcut Conflict"),
                    tr("%1 is assigned to both \"%2\" and \"%3\".")
                        .arg(shortcut, *existing, displayActionText(actions_.at(row))));
                return;
            }
            owners.insert(shortcut, displayActionText(actions_.at(row)));
        }
        bindings.push_back(shortcuts);
    }

    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kSettingsGroup));
    for (int row = 0; row < actions_.size(); ++row) {
        auto* action = actions_.at(row);
        settings.setValue(action->objectName(), bindings.at(row));
        action->setShortcuts(fromPortableStrings(bindings.at(row)));
    }
    settings.endGroup();
    QDialog::accept();
}

void ShortcutEditorDialog::restoreDefaults()
{
    for (int row = 0; row < actions_.size(); ++row) {
        setRowShortcuts(row, actions_.at(row)->property(kDefaultsProperty).toStringList());
    }
}

QStringList ShortcutEditorDialog::shortcutsForRow(const int row) const
{
    QStringList shortcuts;
    for (int binding = 0; binding < bindingColumnCount_; ++binding) {
        const auto* editor = qobject_cast<QKeySequenceEdit*>(table_->cellWidget(row, binding + 1));
        if (editor == nullptr || editor->keySequence().isEmpty()) {
            continue;
        }
        const QString value = editor->keySequence().toString(QKeySequence::PortableText);
        if (!shortcuts.contains(value)) {
            shortcuts.push_back(value);
        }
    }
    return shortcuts;
}

void ShortcutEditorDialog::setRowShortcuts(const int row, const QStringList& shortcuts)
{
    for (int binding = 0; binding < bindingColumnCount_; ++binding) {
        auto* editor = qobject_cast<QKeySequenceEdit*>(table_->cellWidget(row, binding + 1));
        if (editor == nullptr) {
            continue;
        }
        const QKeySequence shortcut = binding < shortcuts.size()
            ? QKeySequence::fromString(shortcuts.at(binding), QKeySequence::PortableText)
            : QKeySequence{};
        editor->setKeySequence(shortcut);
    }
}

} // namespace vidscope::widgets
