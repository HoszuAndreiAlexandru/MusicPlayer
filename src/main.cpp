#include "main_window.h"

#include <QApplication>
#include <QColor>
#include <QPalette>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("MusicPlayer");
    QApplication::setOrganizationName("MusicPlayer");
    app.setStyleSheet(R"(
        QToolTip {
            color: #b3b3b3;
            background-color: #282828;
            border: 1px solid #3e3e3e;
            padding: 5px 7px;
        }
    )");

    QPalette palette = app.palette();
    palette.setColor(QPalette::Window, QColor("#000000"));
    palette.setColor(QPalette::WindowText, QColor("#ffffff"));
    palette.setColor(QPalette::Base, QColor("#181818"));
    palette.setColor(QPalette::AlternateBase, QColor("#202020"));
    palette.setColor(QPalette::ToolTipBase, QColor("#282828"));
    palette.setColor(QPalette::ToolTipText, QColor("#b3b3b3"));
    palette.setColor(QPalette::Text, QColor("#ffffff"));
    palette.setColor(QPalette::Button, QColor("#282828"));
    palette.setColor(QPalette::ButtonText, QColor("#ffffff"));
    palette.setColor(QPalette::BrightText, QColor("#ffffff"));
    palette.setColor(QPalette::Highlight, QColor("#1ed760"));
    palette.setColor(QPalette::HighlightedText, QColor("#000000"));
    palette.setColor(QPalette::PlaceholderText, QColor("#6a6a6a"));
    app.setPalette(palette);

    MainWindow window;

    return app.exec();
}
