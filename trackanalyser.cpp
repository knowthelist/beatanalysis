/*
    Copyright (C) 2011-2014 Mario Stephan <mstephan@shared-files.de>

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

#include "trackanalyser.h"
#include <gst/base/gstadapter.h>
#include <gst/fft/gstfftf32.h>

#include <QtGui>
#if QT_VERSION >= 0x050000
 #include <QtConcurrent/QtConcurrent>
#else
 #include <QtConcurrentRun>
#endif

#include <gst/audio/audio.h>

#define AUDIOFREQ 44100
#define SLICE_SIZE 512
#define BANDS 64

static GstStaticCaps sink_caps = GST_STATIC_CAPS (
    "audio/x-raw, "
    "format = (string) " GST_AUDIO_NE(F32) ", "
    "rate = (int) 44100, "
    "channels = (int) 2 "
);

struct TrackAnalyser_Private
{
        QFutureWatcher<void> watcher;
        QMutex mutex;
        float fft_res;
        float *lastSpectrum;
        QList<float> onsets_All;
        QList<float> onsets_HH;
        QList<float> onsets_BD;
        QList<float> onsets_SD;
        QList<float> peaks;
        int bpm;
        GstElement *conv, *sink, *cutter, *audio, *analysis;
        TrackAnalyser::modeType analysisMode;
        float *xcorr;
        GstAdapter *buffer;
        GstFFTF32 *fft;
        GstFFTF32Complex *freqdata;
};

TrackAnalyser::TrackAnalyser(QWidget *parent) :
        QWidget(parent),
    pipeline(0), m_finished(false)
    , p( new TrackAnalyser_Private )
{
    p->analysisMode == TrackAnalyser::STANDARD;

    p->fft_res = AUDIOFREQ / SLICE_SIZE; //sample rate for fft samples in Hz
    p->lastSpectrum = g_new0 (float, SLICE_SIZE);

    //setenv("GST_DEBUG", "*:3", 1); //unix

    gst_init (0, 0);
    prepare();
    connect(&p->watcher, SIGNAL(finished()), this, SLOT(loadThreadFinished()));

}

void TrackAnalyser::sync_set_state(GstElement* element, GstState state)
{ GstStateChangeReturn res; \
        res = gst_element_set_state (GST_ELEMENT (element), state); \
        if(res == GST_STATE_CHANGE_FAILURE) return; \
        if(res == GST_STATE_CHANGE_ASYNC) { \
                GstState state; \
                        res = gst_element_get_state(GST_ELEMENT (element), &state, NULL, 1000000000/*GST_CLOCK_TIME_NONE*/); \
                        if(res == GST_STATE_CHANGE_FAILURE || res == GST_STATE_CHANGE_ASYNC) return; \
} }

TrackAnalyser::~TrackAnalyser()
{
    cleanup();
    delete p;
    p=0;
}


void cb_newpad_ta (GstElement *decodebin,
                   GstPad     *pad,
                   gpointer    data)
{
    TrackAnalyser* instance = (TrackAnalyser*)data;
            instance->newpad(decodebin, pad, data);
}


void TrackAnalyser::newpad (GstElement *decodebin,
                   GstPad     *pad,
                   gpointer    data)
{
        GstCaps *caps;
        GstStructure *str;
        GstPad *audiopad;

        /* only link once */
        GstElement *audio = gst_bin_get_by_name(GST_BIN(pipeline), "audiobin");
        audiopad = gst_element_get_static_pad (audio, "sink");
        gst_object_unref(audio);

        if (GST_PAD_IS_LINKED (audiopad)) {
                g_object_unref (audiopad);
                return;
        }

        /* check media type */
#ifdef GST_API_VERSION_1
        caps = gst_pad_query_caps (pad,NULL);
#else
        caps = gst_pad_get_caps (pad);
#endif
        str = gst_caps_get_structure (caps, 0);
        if (!g_strrstr (gst_structure_get_name (str), "audio")) {
                gst_caps_unref (caps);
                gst_object_unref (audiopad);
                return;
        }
        gst_caps_unref (caps);

        /* link'n'play */
        gst_pad_link (pad, audiopad);
}

GstBusSyncReply TrackAnalyser::bus_cb (GstBus *bus, GstMessage *msg, gpointer data)
{
    TrackAnalyser* instance = (TrackAnalyser*)data;
            instance->messageReceived(msg);
    return GST_BUS_PASS;
}

void TrackAnalyser::cb_handoff (GstElement *fakesink,
                       GstBuffer   *buffer,
                       GstPad      *pad,
                       gpointer     data)
{
    TrackAnalyser* instance = (TrackAnalyser*)data;
            instance->dataReceived(fakesink, buffer, pad);
}

void TrackAnalyser::cleanup()
{
        if(pipeline) sync_set_state (GST_ELEMENT (pipeline), GST_STATE_NULL);
        if(bus) gst_object_unref (bus);
        if(pipeline) gst_object_unref(G_OBJECT(pipeline));
}

bool TrackAnalyser::prepare()
{
        GstElement *dec, *audio, *audioConvert;
        GstPad *audiopad;
        GstCaps *caps;

        p->buffer = NULL;
        p->fft = gst_fft_f32_new (2 * BANDS, FALSE);
        p->freqdata = g_new (GstFFTF32Complex, BANDS + 1);

        pipeline = gst_pipeline_new ("pipeline");
        bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));

#ifdef GST_API_VERSION_1
        dec = gst_element_factory_make ("decodebin", "decoder");
#else
        dec = gst_element_factory_make ("decodebin2", "decoder");
#endif
        g_signal_connect (dec, "pad-added", G_CALLBACK (cb_newpad_ta), this);
        gst_bin_add (GST_BIN (pipeline), dec);

        audio = gst_bin_new ("audiobin");
        p->conv = gst_element_factory_make ("audioconvert", "conv");
        audioConvert = gst_element_factory_make("audioconvert", "audio_converter");
        p->analysis = gst_element_factory_make ("rganalysis", "analysis");
        p->cutter = gst_element_factory_make ("cutter", "cutter");
        p->sink = gst_element_factory_make ("fakesink", "sink");
        audiopad = gst_element_get_static_pad (p->conv, "sink");

        g_object_set (p->analysis, "message", TRUE, NULL);
        g_object_set (p->analysis, "num-tracks", 1, NULL);
        g_object_set (p->cutter, "threshold-dB", -25.0, NULL);


        g_object_set (G_OBJECT (p->sink), "signal-handoffs", TRUE, NULL);

        g_signal_connect(p->sink,"handoff", G_CALLBACK(cb_handoff), this);

        gst_bin_add_many (GST_BIN (audio), p->conv, audioConvert, p->analysis, p->cutter, p->sink, NULL);

        gst_element_link (p->conv, p->analysis);
        gst_element_link (p->analysis, p->cutter);
        gst_element_link (p->cutter, audioConvert);
        caps = gst_static_caps_get (&sink_caps);
        gst_element_link_filtered (audioConvert, p->sink, caps);
        gst_element_add_pad (audio, gst_ghost_pad_new ("sink", audiopad));

        gst_bin_add (GST_BIN (pipeline), audio);
        p->buffer = gst_adapter_new ();

        GstElement *l_src;
        l_src = gst_element_factory_make ("filesrc", "localsrc");
        gst_bin_add_many (GST_BIN (pipeline), l_src, NULL);
        gst_element_set_state (l_src, GST_STATE_NULL);
        gst_element_link ( l_src,dec);

        gst_caps_unref (caps);
        gst_object_unref (audiopad);

#ifdef GST_API_VERSION_1
        gst_bus_set_sync_handler (bus, bus_cb, this, NULL);
#else
        gst_bus_set_sync_handler (bus, bus_cb, this);
#endif

        return pipeline;
}

float TrackAnalyser::resolution()
{
    return  p->fft_res;
}

int TrackAnalyser::bpm()
{
    return  p->bpm;
}

QList<float> TrackAnalyser::peaks()
{
    return  p->peaks;
}

double TrackAnalyser::gainDB()
{
    return  m_GainDB;
}

double TrackAnalyser::gainFactor()
{
    return pow (10, m_GainDB / 20);
}

QTime TrackAnalyser::startPosition()
{
    return m_StartPosition;
}

QTime TrackAnalyser::endPosition()
{
    return m_EndPosition;
}

void TrackAnalyser::setPosition(QTime position)
{
        int time_milliseconds=QTime(0,0).msecsTo(position);
        gint64 time_nanoseconds=( time_milliseconds * GST_MSECOND );
        gst_element_seek (pipeline, 1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
                                 GST_SEEK_TYPE_SET, time_nanoseconds,
                                 GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
        qDebug() << Q_FUNC_INFO <<":"<<" position="<<position;
}

void TrackAnalyser::open(QUrl url)
{
    //To avoid delays load track in another thread
    qDebug() << Q_FUNC_INFO <<":"<<parentWidget()->objectName()<<" url="<<url;
    QFuture<void> future = QtConcurrent::run( this, &TrackAnalyser::asyncOpen,url);
    p->watcher.setFuture(future);
}

void TrackAnalyser::asyncOpen(QUrl url)
{
    p->mutex.lock();
    m_GainDB = GAIN_INVALID;
    //m_StartPosition = QTime(0,0);
    p->onsets_All.clear();
    p->onsets_BD.clear();
    p->onsets_SD.clear();
    p->onsets_HH.clear();

    sync_set_state (GST_ELEMENT (pipeline), GST_STATE_NULL);


    GstElement *l_src = gst_bin_get_by_name(GST_BIN(pipeline), "localsrc");
    g_object_set (G_OBJECT (l_src), "location", (const char*)url.toLocalFile().toUtf8(), NULL);

    sync_set_state (GST_ELEMENT (pipeline), GST_STATE_PAUSED);

    m_finished=false;

    gst_object_unref(l_src);
    p->mutex.unlock();
}

void TrackAnalyser::loadThreadFinished()
{
    // async load in player done
    qDebug() << Q_FUNC_INFO <<":"<<parentWidget()->objectName()<<" analysisMode="<<p->analysisMode;

    if ( p->analysisMode == TrackAnalyser::TEMPO ){
        //setPosition( m_EndPosition.addSecs(-SCAN_DURATION) );
        //setPosition(m_StartPosition);
    }
    else {
        m_EndPosition=length();
    }
    start();
}

void TrackAnalyser::start()
{
    qDebug() << Q_FUNC_INFO <<":"<<parentWidget()->objectName();
    gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_PLAYING);
}


bool TrackAnalyser::close()
{
    gst_element_set_state (GST_ELEMENT (pipeline), GST_STATE_NULL);
    return true;
}


QTime TrackAnalyser::length()
{
    if (pipeline) {

        gint64 value=0;

#ifdef GST_API_VERSION_1
        if(gst_element_query_duration(pipeline, GST_FORMAT_TIME, &value)) {
#else
        GstFormat fmt = GST_FORMAT_TIME;
        if(gst_element_query_duration(pipeline, &fmt, &value)) {
#endif
            //qDebug() << Q_FUNC_INFO <<  " new value:" <<value;
            m_MaxPosition = QTime(0,0).addMSecs( static_cast<uint>( ( value / GST_MSECOND ) )); // nanosec -> msec
        }
    }
    //qDebug() << Q_FUNC_INFO <<  " return:" <<m_MaxPosition;
    return m_MaxPosition;
}

void TrackAnalyser::dataReceived(GstElement *fakesink,
                       GstBuffer   *buffer,
                       GstPad      *pad)

{
    GstStructure *structure;
    gint channels;
    gint wanted_size;
    gfloat *data;
    GstCaps *caps;

        caps = gst_pad_get_current_caps (pad);
        structure = gst_caps_get_structure (caps, 0);

        gst_structure_get_int (structure, "channels", &channels);
        gst_caps_unref (caps);

        wanted_size = channels * SLICE_SIZE * sizeof (gfloat);
        gst_adapter_push (p->buffer, gst_buffer_ref (buffer));

        // get sample buffer slice
        while ((data = (gfloat *)gst_adapter_map (p->buffer, wanted_size)) != NULL) {

                gfloat *specbuf = g_new0 (gfloat, SLICE_SIZE * 2);

                gint i, j;

                for (i = 0; i < SLICE_SIZE; i++) {
                    gfloat avg = 0.0f;

                    // get mono signal
                    for (j = 0; j < channels; j++) {
                        gfloat sample = data[i * channels + j];

                        avg += sample;
                    }

                    avg /= channels;
                    specbuf[i] = avg;

                }

                //make Fast Fourier transform
                gst_fft_f32_window (p->fft, specbuf, GST_FFT_WINDOW_HAMMING);
                gst_fft_f32_fft (p->fft, specbuf, p->freqdata);

                float flux_all,flux_BD,flux_SD,flux_HH;
                flux_all = flux_BD = flux_SD = flux_HH = 0;
                for (i = 0; i < BANDS; i++) {
                    gfloat val;

                    val = p->freqdata[i].r * p->freqdata[i].r;
                    val += p->freqdata[i].i * p->freqdata[i].i;
                    val = qSqrt(val);

                    float value = (val - p->lastSpectrum[i] );

                    p->lastSpectrum[i] = val;
                    // Spectral Flux for interesting frequencies
                    flux_all += value < 0? 0: value;
                    if (i>=0 && i<4)
                        flux_BD += value < 0? 0: value;
                    if (i>10 && i<15)
                        flux_SD += value < 0? 0: value;
                    if (i>20)
                        flux_HH += value < 0? 0: value;

                }
                p->onsets_All.append( flux_all );
                p->onsets_BD.append( flux_BD );
                p->onsets_SD.append( flux_SD );
                p->onsets_HH.append( flux_HH );

                g_free (specbuf);

                gst_adapter_unmap (p->buffer);
                gst_adapter_flush (p->buffer, wanted_size);
            }

}

void TrackAnalyser::messageReceived(GstMessage *message)
{
        switch (GST_MESSAGE_TYPE (message)) {
        case GST_MESSAGE_ERROR: {
                GError *err;
                gchar *debug;
                gst_message_parse_error (message, &err, &debug);
                QString str;
                str = "Error #"+QString::number(err->code)+" in module "+QString::number(err->domain)+"\n"+QString::fromUtf8(err->message);
                if(err->code == 6 && err->domain == 851) {
                        str += "\nMay be you should to install gstreamer0.10-plugins-ugly or gstreamer0.10-plugins-bad";
                }
                qDebug()<< "Gstreamer error:"<< str;
                g_error_free (err);
                g_free (debug);
                need_finish();
                break;
        }
        case GST_MESSAGE_EOS:{
                qDebug() << Q_FUNC_INFO <<":"<<parentWidget()->objectName()<<" End of track reached";
                need_finish();
                break;
        }
        case GST_MESSAGE_ELEMENT:{

                const GstStructure *s = gst_message_get_structure (message);
                const gchar *name = gst_structure_get_name (s);
                GstClockTime timestamp;
                gst_structure_get_clock_time (s, "timestamp", &timestamp);

                // data for Start and End time detection
                if (strcmp (name, "cutter") == 0) {

                    const GValue *value;
                    value=gst_structure_get_value (s, "above");
                    bool isSilent=!g_value_get_boolean(value);

                    //if we detect a falling edge, set EndPostion to this
                    if (isSilent)
                        m_EndPosition=QTime(0,0).addMSecs( static_cast<uint>( ( timestamp / GST_MSECOND ) )); // nanosec -> msec
                    else
                    {
                        //if this is the first rising edge, set StartPosition
                        if (m_StartPosition==QTime(0,0) && m_EndPosition==m_MaxPosition)
                            m_StartPosition=QTime(0,0).addMSecs( static_cast<uint>( ( timestamp / GST_MSECOND ) )); // nanosec -> msec

                        //if we detect a rising edge, set EndPostion to track end
                        m_EndPosition=length();
                    }
                    //qDebug() << Q_FUNC_INFO <<QTime(0,0).addMSecs( static_cast<uint>( ( timestamp / GST_MSECOND ) ))<< " silent:" << isSilent;
                }
            break;
          }

        case GST_MESSAGE_TAG:{

                GstTagList *tags = NULL;
                gst_message_parse_tag (message, &tags);
                if (gst_tag_list_get_double (tags, GST_TAG_TRACK_GAIN, &m_GainDB))
                {
                    qDebug() << Q_FUNC_INFO << ": Gain-db:" << m_GainDB;
                    qDebug() << Q_FUNC_INFO << ": Gain-norm:" << pow (10, m_GainDB / 20);
                }
            }

        default:
                break;
        }

}

void TrackAnalyser::need_finish()
{
    m_finished=true;
    Q_EMIT finishGain();

    p->bpm = qRound(detectTempo( p->onsets_All));

    //ToDo:analyze found bpm value according tempo-harmonics issue
    // do we have the base beat or just the 2nd harmonic

    /*
     //tempo-harmonics issue
    if ( bpm < 72.0 && p->xcorr[peak/2] > 0.6 * p->xcorr[peak] )
    {
        bpm = 60.0 * p->fft_res / peak * 2.0f;
        qDebug() << Q_FUNC_INFO << "guess bpm:"<<bpm;
    }
    */


    Q_EMIT finishTempo();

    //ToDo:beat tracking - find position of beat 1,2,3 and 4
}

float TrackAnalyser::detectTempo(QList<float> onsets)
{
    int THRESHOLD_WINDOW_SIZE = 10;
    float MULTIPLIER = 2.0f;
    QList<float> prunedSpectralFlux;
    QList<float> threshold;
    p->peaks.clear();
    int pcount=0;
    int minBpm=60;
    int maxBpm=200;

    //calculate the running average for spectral flux.
    for( int i = 0; i < onsets.size(); i++ )
    {
       int start = qMax( 0, i - THRESHOLD_WINDOW_SIZE );
       int end = qMin( onsets.size() - 1, i + THRESHOLD_WINDOW_SIZE );
       float mean = 0;
       for( int j = start; j <= end; j++ )
          mean += onsets.at(j);
       mean /= (end - start);
       threshold.append( mean * MULTIPLIER );
    }

    //take only the signifikat onsets above threshold
    for( int i = 0; i < threshold.size(); i++ )
    {
       if( threshold.at(i) <= onsets.at(i) )
          prunedSpectralFlux.append( onsets.at(i) - threshold.at(i) );
       else
          prunedSpectralFlux.append( (float)0 );
    }

    //peak detection
    for( int i = 0; i < prunedSpectralFlux.size() - 1; i++ )
    {
       if( prunedSpectralFlux.at(i) > prunedSpectralFlux.at(i+1) ){
          p->peaks.append( prunedSpectralFlux.at(i) );
          pcount++;
       }
       else
          p->peaks.append( (float)0 );
    }

    //use autocorrelation to retrieve time periode of peaks
    int frames = p->peaks.count();
    p->xcorr = new float[frames];
    memset(p->xcorr, 0, frames * sizeof(float));

    int maxLag = p->fft_res * 60 / minBpm;
    int minLag = p->fft_res * 60 / maxBpm;
    int peak = AutoCorrelation(p->peaks, frames, minLag, maxLag);
    float bpm = 60.0 * p->fft_res / peak;
    qDebug() << Q_FUNC_INFO << "autocorrelation bpm:"<<bpm<< " corr:"<<p->xcorr[peak];
    qDebug() << Q_FUNC_INFO << "autocorrelation 2xbpm:"<< 60.0 * p->fft_res / peak * 2.0f << " corr:"<<p->xcorr[peak/2];
    qDebug() << Q_FUNC_INFO << "autocorrelation 0.5xbpm:"<< 60.0 * p->fft_res / peak * 0.5f << " corr:"<<p->xcorr[peak*2];
    qDebug() << Q_FUNC_INFO << "autocorrelation density:"<<pcount/p->fft_res;
    qDebug() << Q_FUNC_INFO << "autocorrelation count:"<<pcount;

    return bpm;
}

int TrackAnalyser::AutoCorrelation( QList<float> buffer, int frames, int minLag, int maxLag)
{
    float maxCorr = 0;
    int optiLag = 0;

    if (frames > buffer.count()) frames=buffer.count();

    for (int lag = minLag; lag < maxLag; lag++)
    {
        for (int i = 0; i < frames-lag; i++)
        {
            p->xcorr[lag] += (buffer.at(i+lag) * buffer.at(i));
        }

        float bpm = p->fft_res * 60.0 / lag;

        if (p->xcorr[lag] > maxCorr)
        {
            qDebug() << Q_FUNC_INFO << "corr: "<<p->xcorr[lag] << " lag: "<< lag <<" bpm:"<<bpm;
            maxCorr = p->xcorr[lag];
            optiLag = lag;
        }

    }

    return optiLag;
}
