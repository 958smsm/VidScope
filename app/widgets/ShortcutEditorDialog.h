#pragma once

#include <QtCore/QList>
#include <QtCore/QStringList>
#include <QtGui/QKeySequence>
#include <QtWidgets/QDialog>

class QAction;
class QTableWidget;

namespace vidscope::widgets {

class ShortcutEditorDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ShortcutEditorDialog(const QList<QAction*>& actions, QWidget* parent = nullptr);

    static void configureAction(
        QAction* action,
        const QList<QKeySequence>& defaultShortcuts);

public slots:
    void accept() override;

private slots:
    void restoreDefaults();

private:
    [[nodiscard]] QStringList shortcutsForRow(int row) const;
    void setRowShortcuts(int row, const QStringList& shortcuts);

    QList<QAction*> actions_;
    QTableWidget* table_ = nullptr;
    int bindingColumnCount_ = 0;
};

} // namespace vidscope::widgets
