#include "SchematicCanvas.h"

#include <cmath>
#include <cassert>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QGraphicsPathItem>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QGraphicsSceneMouseEvent>
#include <iostream>

#include "AddNodeMenu.h"
#include "compiler/runtime/Runtime.h"
#include "editor/AxiomApplication.h"
#include "editor/model/schematic/Schematic.h"
#include "editor/model/node/CustomNode.h"
#include "editor/model/node/GroupNode.h"
#include "editor/model/control/NodeNumControl.h"
#include "editor/model/Project.h"
#include "../node/NodeItem.h"
#include "../connection/WireItem.h"
#include "../IConnectable.h"
#include "../FloatingValueEditor.h"

using namespace AxiomGui;
using namespace AxiomModel;

QSize SchematicCanvas::nodeGridSize = QSize(50, 50);

QSize SchematicCanvas::controlGridSize = QSize(25, 25);

int SchematicCanvas::wireZVal = 0;
int SchematicCanvas::activeWireZVal = 1;
int SchematicCanvas::nodeZVal = 2;
int SchematicCanvas::activeNodeZVal = 3;
int SchematicCanvas::panelZVal = 4;
int SchematicCanvas::selectionZVal = 5;

SchematicCanvas::SchematicCanvas(SchematicPanel *panel, Schematic *schematic) : panel(panel), schematic(schematic) {
    // build selection
    auto selectionPen = QPen(QColor::fromRgb(52, 152, 219));
    auto selectionBrush = QBrush(QColor::fromRgb(52, 152, 219, 50));

    selectionPath = addPath(QPainterPath(), selectionPen, selectionBrush);
    selectionPath->setVisible(false);
    selectionPath->setZValue(selectionZVal);

    // create items for all nodes & wires that already exist
    for (const auto &item : schematic->items()) {
        if (auto node = dynamic_cast<Node *>(item.get())) {
            addNode(node);
        }
    }

    for (const auto &wire : schematic->wires()) {
        addWire(wire.get());
    }

    // connect to model
    connect(schematic, &Schematic::itemAdded,
            [this](AxiomModel::GridItem *item) {
                if (auto node = dynamic_cast<Node *>(item)) {
                    addNode(node);
                }
            });
    connect(schematic, &Schematic::wireAdded,
            this, &SchematicCanvas::addWire);

    // start runtime update timer
    auto timer = new QTimer(this);
    connect(timer, &QTimer::timeout,
            this, &SchematicCanvas::doRuntimeUpdate);
    timer->start(16);
}

QPoint SchematicCanvas::nodeRealPos(const QPoint &p) {
    return {
        p.x() * SchematicCanvas::nodeGridSize.width(),
        p.y() * SchematicCanvas::nodeGridSize.height()
    };
}

QSize SchematicCanvas::nodeRealSize(const QSize &s) {
    return {
        s.width() * SchematicCanvas::nodeGridSize.width(),
        s.height() * SchematicCanvas::nodeGridSize.height()
    };
}

QPoint SchematicCanvas::controlRealPos(const QPoint &p) {
    return {
        p.x() * SchematicCanvas::controlGridSize.width(),
        p.y() * SchematicCanvas::controlGridSize.height()
    };
}

QPointF SchematicCanvas::controlRealPos(const QPointF &p) {
    return {
        p.x() * SchematicCanvas::controlGridSize.width(),
        p.y() * SchematicCanvas::controlGridSize.height()
    };
}

QSize SchematicCanvas::controlRealSize(const QSize &s) {
    return {
        s.width() * SchematicCanvas::controlGridSize.width(),
        s.height() * SchematicCanvas::controlGridSize.height()
    };
}

void SchematicCanvas::startConnecting(IConnectable *control) {
    if (isConnecting) return;

    isConnecting = true;
    connectionSink = std::make_unique<ConnectionSink>(control->sink()->type, nullptr);
    connectionSink->setPos(control->sink()->pos(), control->sink()->subPos());
    connectionSink->setActive(true);
    connectionWire = schematic->connectSinks(control->sink(), connectionSink.get());
    assert(connectionWire != nullptr);

    connect(connectionWire, &ConnectionWire::removed,
            [this]() { isConnecting = false; });
}

void SchematicCanvas::updateConnecting(QPointF mousePos) {
    if (!isConnecting) return;

    auto currentItem = itemAt(mousePos, QTransform());
    auto connectable = dynamic_cast<IConnectable *>(currentItem);
    if (connectable && connectable->sink()->type == connectionSink->type) {
        connectionSink->setPos(connectable->sink()->pos(), connectable->sink()->subPos());
    } else {
        connectionSink->setPos(
            QPoint(
                (int) (mousePos.x() / SchematicCanvas::nodeGridSize.width()),
                (int) (mousePos.y() / SchematicCanvas::nodeGridSize.height())
            ),
            QPointF(
                mousePos.x() / SchematicCanvas::controlGridSize.width(),
                mousePos.y() / SchematicCanvas::controlGridSize.height()
            )
        );
    }
}

void SchematicCanvas::endConnecting(QPointF mousePos) {
    if (!isConnecting) return;

    auto currentItem = itemAt(mousePos, QTransform());

    if (auto connectable = dynamic_cast<IConnectable *>(currentItem)) {
        isConnecting = false;

        // if the sinks are already connected, remove the connection
        if (auto connectingWire = connectionWire->sinkA->getConnectingWire(connectable->sink())) {
            connectingWire->remove();
        } else {
            schematic->connectSinks(connectionWire->sinkA, connectable->sink());
        }

        schematic->project()->build();
    } else {
        // todo: create JunctionNode if not currentItem is null
    }

    connectionSink->setActive(false);
    connectionWire->remove();
    connectionSink.reset();
}

void SchematicCanvas::cancelConnecting() {
    if (!isConnecting) return;

    connectionWire->remove();
}

void SchematicCanvas::addNode(AxiomModel::Node *node) {
    auto item = new NodeItem(node, this);
    item->setZValue(nodeZVal);
    addItem(item);
}

void SchematicCanvas::newNode(QPointF scenePos, QString name, bool group) {
    auto targetPos = QPoint(
        qRound((float) scenePos.x() / SchematicCanvas::nodeGridSize.width()),
        qRound((float) scenePos.y() / SchematicCanvas::nodeGridSize.height())
    );

    if (group) {
        DO_ACTION(schematic->project()->history, HistoryList::ActionType::CREATE_GROUP_NODE, {
            schematic->addNode(Node::Type::GROUP, name, targetPos);
        });
    } else {
        DO_ACTION(schematic->project()->history, HistoryList::ActionType::CREATE_CUSTOM_NODE, {
            schematic->addNode(Node::Type::CUSTOM, name, targetPos);
        });
    }
}

void SchematicCanvas::addWire(AxiomModel::ConnectionWire *wire) {
    auto item = new WireItem(this, wire);
    item->setZValue(wireZVal);
    addItem(item);
}

void SchematicCanvas::doRuntimeUpdate() {
    schematic->doRuntimeUpdate();
}

void SchematicCanvas::drawBackground(QPainter *painter, const QRectF &rect) {
    drawGrid(painter, rect, nodeGridSize, QColor::fromRgb(34, 34, 34), 2);
}

void SchematicCanvas::mousePressEvent(QGraphicsSceneMouseEvent *event) {
    QGraphicsScene::mousePressEvent(event);
    if (event->isAccepted() && itemAt(event->scenePos(), QTransform()) != selectionPath) return;

    switch (event->button()) {
        case Qt::LeftButton:
            leftMousePressEvent(event);
            break;
        default:
            break;
    }
}

void SchematicCanvas::mouseReleaseEvent(QGraphicsSceneMouseEvent *event) {
    QGraphicsScene::mouseReleaseEvent(event);
    if (event->isAccepted() && itemAt(event->scenePos(), QTransform()) != selectionPath) return;

    switch (event->button()) {
        case Qt::LeftButton:
            leftMouseReleaseEvent(event);
            break;
        default:
            break;
    }
}

void SchematicCanvas::mouseMoveEvent(QGraphicsSceneMouseEvent *event) {
    QGraphicsScene::mouseMoveEvent(event);
    if (event->isAccepted() && itemAt(event->scenePos(), QTransform()) != selectionPath) return;

    event->ignore();

    if (isSelecting) {
        selectionPoints.append(event->scenePos());

        auto path = QPainterPath();
        path.moveTo(selectionPoints.first());
        for (auto i = 1; i < selectionPoints.size(); i++) {
            path.lineTo(selectionPoints[i]);
        }
        path.closeSubpath();

        selectionPath->setPath(path);
        selectionPath->setVisible(true);

        auto selectItems = items(path);

        std::set<AxiomModel::GridItem *> newSelectedItems;
        for (auto &item : selectItems) {
            auto nodeItem = dynamic_cast<NodeItem *>(item);
            if (!nodeItem) continue;
            newSelectedItems.emplace(nodeItem->node);
        }

        std::vector<AxiomModel::GridItem *> edgeItems;
        std::set_symmetric_difference(lastSelectedItems.begin(), lastSelectedItems.end(),
                                      newSelectedItems.begin(), newSelectedItems.end(), std::back_inserter(edgeItems));

        for (auto &item : edgeItems) {
            if (item->isSelected()) {
                item->deselect();
            } else {
                item->select(false);
            }
        }

        lastSelectedItems = newSelectedItems;

        event->accept();
    }
}

void SchematicCanvas::keyPressEvent(QKeyEvent *event) {
    if (focusItem()) {
        QGraphicsScene::keyPressEvent(event);
    } else if (event->matches(QKeySequence::Delete)) {
        DO_ACTION(schematic->project()->history, HistoryList::ActionType::DELETE_SELECTED_ITEMS, {
            schematic->deleteSelectedItems();
        });
    }
}

void SchematicCanvas::contextMenuEvent(QGraphicsSceneContextMenuEvent *event) {
    QGraphicsScene::contextMenuEvent(event);
    if (event->isAccepted()) return;

    auto scenePos = event->scenePos();
    AddNodeMenu menu(schematic, "");

    connect(&menu, &AddNodeMenu::newNodeAdded,
            [this, scenePos]() {
                auto editor = new FloatingValueEditor("New Node", scenePos);
                addItem(editor);
                connect(editor, &FloatingValueEditor::valueSubmitted,
                        [this, scenePos](QString value) {
                            newNode(scenePos, value, false);
                        });
            });
    connect(&menu, &AddNodeMenu::newGroupAdded,
            [this, scenePos]() {
                auto editor = new FloatingValueEditor("New Group", scenePos);
                addItem(editor);
                connect(editor, &FloatingValueEditor::valueSubmitted,
                        [this, scenePos](QString value) {
                            newNode(scenePos, value, true);
                        });
            });

    menu.exec(event->screenPos());
}

void SchematicCanvas::leftMousePressEvent(QGraphicsSceneMouseEvent *event) {
    isSelecting = true;
    if (!(event->modifiers() & Qt::ShiftModifier)) {
        schematic->deselectAll();
        if (focusItem()) focusItem()->clearFocus();
    }
    lastSelectedItems.clear();
    selectionPoints.append(event->scenePos());
    event->accept();
}

void SchematicCanvas::leftMouseReleaseEvent(QGraphicsSceneMouseEvent *event) {
    if (!isSelecting) {
        event->ignore();
        return;
    }

    isSelecting = false;
    selectionPoints.clear();
    selectionPath->setVisible(false);
    event->accept();
}

void SchematicCanvas::drawGrid(QPainter *painter, const QRectF &rect, const QSize &size, const QColor &color,
                               qreal pointSize) {
    QPointF topLeft = rect.topLeft();
    topLeft.setX(std::floor(topLeft.x() / size.width()) * size.width());
    topLeft.setY(std::floor(topLeft.y() / size.height()) * size.height());

    QPointF bottomRight = rect.bottomRight();
    bottomRight.setX(std::ceil(bottomRight.x() / size.width()) * size.width());
    bottomRight.setY(std::ceil(bottomRight.y() / size.height()) * size.height());

    auto drawPen = QPen(color);
    drawPen.setWidthF(pointSize);
    painter->setPen(drawPen);

    for (auto x = topLeft.x(); x < bottomRight.x(); x += size.width()) {
        for (auto y = topLeft.y(); y < bottomRight.y(); y += size.height()) {
            painter->drawPoint((int) x + 1, (int) y + 1);
        }
    }
}
