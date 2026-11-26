#include "bitstreamparser.h"
#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("BitScope"));
    QApplication::setApplicationVersion(QStringLiteral("1.0.0"));
    QApplication::setOrganizationName(QStringLiteral("BitScope"));
    QApplication::setOrganizationDomain(QStringLiteral("bitscope.local"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("跨平台 MP4、H.264 / H.265 / MPEG-4 Part 2 / AV1 码流分析器"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption analyzeOption(QStringList{QStringLiteral("a"), QStringLiteral("analyze")}, QStringLiteral("解析文件并以 JSON 输出摘要，不显示界面"), QStringLiteral("file"));
    QCommandLineOption screenshotOption(QStringLiteral("screenshot"), QStringLiteral("启动后将窗口截图保存为 PNG（用于界面测试）"), QStringLiteral("png"));
    QCommandLineOption fileViewOption(QStringLiteral("file-view"), QStringLiteral("打开 MP4 后直接显示文件 Box 视图"));
    parser.addOption(analyzeOption);
    parser.addOption(screenshotOption);
    parser.addOption(fileViewOption);
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("启动后打开的 MP4、H.264、H.265、MPEG-4 Part 2 或 AV1 文件"), QStringLiteral("[file]"));
    parser.process(app);

    if (parser.isSet(analyzeOption)) {
        const auto stream = bitscope::BitstreamParser::parseFile(parser.value(analyzeOption));
        QJsonObject result{{QStringLiteral("file"), stream->filePath}, {QStringLiteral("error"), stream->error},
            {QStringLiteral("codec"), stream->codecName()}, {QStringLiteral("format"), stream->format},
            {QStringLiteral("container"), stream->containerFile}, {QStringLiteral("top_level_boxes"), stream->boxes.size()},
            {QStringLiteral("width"), stream->width}, {QStringLiteral("height"), stream->height},
            {QStringLiteral("bit_depth"), stream->bitDepth}, {QStringLiteral("chroma_format"), stream->chroma},
            {QStringLiteral("profile_name"), stream->profileName},
            {QStringLiteral("fps"), stream->fps}, {QStringLiteral("frames"), stream->frames.size()},
            {QStringLiteral("nals"), stream->nals.size()}, {QStringLiteral("units"), stream->nals.size()}, {QStringLiteral("duration"), stream->duration}};
        QJsonObject frameTypes;
        for (const auto &frame : stream->frames)
            frameTypes.insert(frame.displayType, frameTypes.value(frame.displayType).toInt() + 1);
        int parseErrors = 0;
        for (const auto &unit : stream->nals)
            for (const auto &syntaxField : unit.fields)
                if (syntaxField.name == QStringLiteral("parse_error")) ++parseErrors;
        result.insert(QStringLiteral("frame_types"), frameTypes);
        result.insert(QStringLiteral("unit_parse_errors"), parseErrors);
        if (!stream->frames.isEmpty()) {
            const auto &first = stream->frames.first();
            result.insert(QStringLiteral("first_frame_type"), first.displayType);
            result.insert(QStringLiteral("first_frame_structure"), first.structure);
            result.insert(QStringLiteral("first_frame_slices"), first.sliceNalIndices.size());
            result.insert(QStringLiteral("first_frame_truncated"), first.truncatedStart);
        }
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Indented);
        return stream->error.isEmpty() ? 0 : 2;
    }

    bitscope::MainWindow window;
    window.show();
    const QStringList positional = parser.positionalArguments();
    if (!positional.isEmpty()) window.openFile(QFileInfo(positional.first()).absoluteFilePath());
    if (parser.isSet(fileViewOption)) window.requestFileView();
    if (parser.isSet(screenshotOption)) {
        const QString output = QFileInfo(parser.value(screenshotOption)).absoluteFilePath();
        QTimer::singleShot(8000, &window, [&window, &app, output] {
            window.grab().save(output, "PNG");
            app.quit();
        });
    }
    return app.exec();
}
