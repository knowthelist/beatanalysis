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

#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QDesktopServices>
#include <QFileDialog>
#include <QUrl>
#include <QTimer>
#include <QLabel>
#include <QGraphicsLineItem>
#include <QGraphicsScene>

#include "trackanalyser.h"
#include "player.h"


//Evaluation project to improve the trackanalyser of Knowthelist

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    //the analyser which needs improment
    trackanalyser = new TrackAnalyser(this);
    connect(trackanalyser, SIGNAL(finishTempo()),this,SLOT(analyseTempoFinished()));

    //a player to see and hear
    player = new Player(this);
    player->prepare();

    //visualization
    h = ui->graphicsView->sizeHint().height();
    scene = new QGraphicsScene(this);
    ui->graphicsView->setScene(scene);
    linePosi = new QGraphicsLineItem(NULL,scene);

    //timer for the position drawer
    timerPosition = new QTimer(this);
    timerPosition->stop();
    timerPosition->setInterval(10);
    connect( timerPosition, SIGNAL(timeout()), SLOT(timerPosition_timeOut()) );

    //latency of the soundcard output
    delay = 200; //msec
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::analyseTempoFinished()
{
    qDebug() << " resolution:" <<trackanalyser->resolution();
    qDebug() << " onset count:" <<trackanalyser->peaks().count();

    // Show BPM Result
    ui->lblBpm->setText(QString::number(trackanalyser->bpm()));

    // Draw found onsets
    scene->clear();
    for ( int i=1;i<trackanalyser->peaks().count();i++ ) {
         QPen outlinePen(Qt::blue);
         scene->addLine( QLineF( i, h, i, h-trackanalyser->peaks().at(i)*2 ), outlinePen);
    }

}

void MainWindow::timerPosition_timeOut()
{
    int posi_ms = QTime(0,0).msecsTo(player->position());
    int posi_idx = (posi_ms + delay) * trackanalyser->resolution() / 1000;
    qDebug() << " posi_ms:" <<posi_ms<< " posi_idx:" <<posi_idx<<" length"<< QTime(0,0).msecsTo(player->length()) * trackanalyser->resolution() /1000;

    //Draw current position while playing
    QGraphicsLineItem* li =  dynamic_cast<QGraphicsLineItem*>(scene->items().at(0));
    if ( li){
        li->setPen(QPen(Qt::red));
        li->setLine( posi_idx, h, posi_idx, h/2);
    }
}

void MainWindow::on_pushAnalyse_clicked()
{
    trackanalyser->open(QUrl(ui->lineEdit->text()));
}

void MainWindow::on_pushPlay_clicked()
{
    if (player->isPlaying())
    {
        player->stop();
        timerPosition->stop();
    }
    else
    {
        player->open(QUrl(ui->lineEdit->text()));
        player->play();
        timerPosition->start();
    }
}

void MainWindow::on_pushOpen_clicked()
{
#if QT_VERSION >= 0x050000
    QString pathName = QStandardPaths::standardLocations(QStandardPaths::MusicLocation).at(0);
#else
    QString pathName = QDesktopServices::storageLocation(QDesktopServices::MusicLocation);
#endif
    QString fileName = QFileDialog::getOpenFileName(this,
          tr("Open Song"), pathName, tr("Music Files (*.mp3)"));
    ui->lineEdit->setText(fileName);
}
