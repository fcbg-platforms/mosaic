#include <QApplication>
#include "core/application.hpp"

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("MOSAIC");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("CSRU");

    mosaic::Application mosaic;
    mosaic.initialize();

    return app.exec();
}
