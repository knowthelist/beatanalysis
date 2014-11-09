#ifndef MAINWINDOW_H
#define MAINWINDOW_H

/*
    Copyright (C) 2014 Mario Stephan <mstephan@shared-files.de>

    This library is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <QMainWindow>

#include "trackanalyser.h"
#include "player.h"

#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsLineItem>

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT
    
public:
    explicit MainWindow(QWidget *parent = 0);
    ~MainWindow();
    
private slots:
    void analyseTempoFinished();
    void timerPosition_timeOut();

    void on_pushAnalyse_clicked();

    void on_pushPlay_clicked();

    void on_pushOpen_clicked();

private:
    Ui::MainWindow *ui;
    TrackAnalyser *trackanalyser;
    Player *player;
    QTimer *timerPosition;
    int h;
    int delay;

    QGraphicsScene *scene;
    QGraphicsLineItem* linePosi;

};

#endif // MAINWINDOW_H
