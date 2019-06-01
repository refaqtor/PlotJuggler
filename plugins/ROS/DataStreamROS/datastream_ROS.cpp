#include "datastream_ROS.h"
#include <QTextStream>
#include <QFile>
#include <QMessageBox>
#include <QDebug>
#include <thread>
#include <mutex>
#include <chrono>
#include <thread>
#include <QProgressDialog>
#include <QtGlobal>
#include <QApplication>
#include <QProcess>
#include <QCheckBox>
#include <QSettings>
#include <QFileDialog>
#include <ros/callback_queue.h>
#include <rosbag/bag.h>
#include <topic_tools/shape_shifter.h>
#include <ros/transport_hints.h>

#include "../dialog_select_ros_topics.h"
#include "../rule_editing.h"
#include "../qnodedialog.h"
#include "../shape_shifter_factory.hpp"

DataStreamROS::DataStreamROS():
    DataStreamer(),
    _node(nullptr),
    _action_saveIntoRosbag(nullptr),
    _clock_time(0)
{
    _running = false;
    _initial_time = std::numeric_limits<double>::max();
    _periodic_timer = new QTimer();
    connect( _periodic_timer, &QTimer::timeout,
             this, &DataStreamROS::timerCallback);
}

void DataStreamROS::clockCallback(const rosgraph_msgs::Clock::ConstPtr& msg)
{
    if( ( msg->clock - _clock_time ) < ros::Duration(-1,0) && _action_clearBuffer->isChecked() )
    {
        emit clearBuffers();
    }
    _clock_time = msg->clock;
}

void DataStreamROS::topicCallback(const topic_tools::ShapeShifter::ConstPtr& msg,
                                  const std::string &topic_name)
{
    if( !_running ){
        return;
    }

    using namespace RosIntrospection;
    const auto&  md5sum     =  msg->getMD5Sum();
    const auto&  datatype   =  msg->getDataType();
    const auto&  definition =  msg->getMessageDefinition() ;

    // register the message type
    _ros_parser.registerSchema( topic_name, md5sum,
                                RosIntrospection::ROSType(datatype),
                                definition);

    RosIntrospectionFactory::registerMessage(topic_name, md5sum, datatype, definition );

    //------------------------------------

    // it is more efficient to recycle this elements
    static std::vector<uint8_t> buffer;
    
    buffer.resize( msg->size() );
    
    ros::serialization::OStream stream(buffer.data(), buffer.size());
    msg->write(stream);

    double msg_time = ros::Time::now().toSec();

    RawMessage buffer_view( buffer );
    _ros_parser.pushRawMessage( topic_name, buffer_view, msg_time );

    std::lock_guard<std::mutex> lock( mutex() );
    const std::string full_prefix = _prefix + topic_name;

    // adding raw serialized msg for future uses.
    // do this before msg_time normalization
    {
        auto plot_pair = dataMap().user_defined.find( full_prefix );
        if( plot_pair == dataMap().user_defined.end() )
        {
            plot_pair = dataMap().addUserDefined( full_prefix );
        }
        PlotDataAny& user_defined_data = plot_pair->second;
        user_defined_data.pushBack( PlotDataAny::Point(msg_time, nonstd::any(std::move(buffer)) ));
    }

    _ros_parser.extractData(dataMap(), full_prefix);

    //------------------------------
    {
        int& index = _msg_index[topic_name];
        index++;
        const std::string key = full_prefix + ("/_MSG_INDEX_") ;
        auto index_it = dataMap().numeric.find(key);
        if( index_it == dataMap().numeric.end())
        {
            index_it = dataMap().addNumeric( key );
        }
        index_it->second.pushBack( PlotData::Point(msg_time, index) );
    }
}

void DataStreamROS::extractInitialSamples()
{
    using namespace std::chrono;
    milliseconds wait_time_ms(1000);

    QProgressDialog progress_dialog;
    progress_dialog.setLabelText( "Collecting ROS topic samples to understand data layout. ");
    progress_dialog.setRange(0, wait_time_ms.count());
    progress_dialog.setAutoClose(true);
    progress_dialog.setAutoReset(true);

    progress_dialog.show();

    auto start_time = system_clock::now();

    while ( system_clock::now() - start_time < (wait_time_ms) )
    {
        ros::getGlobalCallbackQueue()->callAvailable(ros::WallDuration(0.1));
        int i = duration_cast<milliseconds>(system_clock::now() - start_time).count() ;
        progress_dialog.setValue( i );
        QApplication::processEvents();
        if( progress_dialog.wasCanceled() )
        {
            break;
        }
    }

    if( progress_dialog.wasCanceled() == false )
    {
        progress_dialog.cancel();
    }
}

void DataStreamROS::timerCallback()
{
    if( _running && ros::master::check() == false
            && !_roscore_disconnection_already_notified)
    {
        auto ret = QMessageBox::warning(nullptr,
                                        tr("Disconnected!"),
                                        tr("The roscore master cannot be detected.\n\n"
                                           "Do you want to try reconnecting to it? \n\n"
                                           "NOTE: if you select CONTINUE, you might need"
                                           " to stop and restart this plugin."),
                                        tr("Stop Plugin"),
                                        tr("Try reconnect"),
                                        tr("Continue"),
                                        0);
        _roscore_disconnection_already_notified = ( ret == 2 );
        if( ret == 1 )
        {
            this->shutdown();
            _node =  RosManager::getNode();

            if( !_node ){
                emit connectionClosed();
                return;
            }
            subscribe();

            _running = true;
            _spinner = std::make_shared<ros::AsyncSpinner>(1);
            _spinner->start();
            _periodic_timer->start();
        }
        else if( ret == 0)
        {
            this->shutdown();
            emit connectionClosed();
        }
    }
}

void DataStreamROS::saveIntoRosbag()
{
    std::lock_guard<std::mutex> lock( mutex() );
    if( dataMap().user_defined.empty()){
        QMessageBox::warning(nullptr, tr("Warning"), tr("Your buffer is empty. Nothing to save.\n") );
        return;
    }

    QFileDialog saveDialog;
    saveDialog.setAcceptMode(QFileDialog::AcceptSave);
    saveDialog.setDefaultSuffix("bag");
    saveDialog.exec();

    if(saveDialog.result() != QDialog::Accepted || saveDialog.selectedFiles().empty())
    {
        return;
    }

    QString fileName = saveDialog.selectedFiles().first();

    if( fileName.size() > 0)
    {
        rosbag::Bag rosbag(fileName.toStdString(), rosbag::bagmode::Write );

        for (const auto& it: dataMap().user_defined )
        {
            const std::string& topicname = it.first;
            const auto& plotdata = it.second;

            auto registered_msg_type = RosIntrospectionFactory::get().getShapeShifter(topicname);
            if(!registered_msg_type) continue;

            RosIntrospection::ShapeShifter msg;
            msg.morph(registered_msg_type->getMD5Sum(),
                      registered_msg_type->getDataType(),
                      registered_msg_type->getMessageDefinition());

            for (int i=0; i< plotdata.size(); i++)
            {
                const auto& point = plotdata.at(i);
                const PlotDataAny::TimeType msg_time  = point.x;
                const nonstd::any& type_erased_buffer = point.y;

                if(type_erased_buffer.type() != typeid( std::vector<uint8_t> ))
                {
                    // can't cast to expected type
                    continue;
                }

                std::vector<uint8_t> raw_buffer =  nonstd::any_cast<std::vector<uint8_t>>( type_erased_buffer );
                ros::serialization::IStream stream( raw_buffer.data(), raw_buffer.size() );
                msg.read( stream );

                rosbag.write( topicname, ros::Time(msg_time), msg);
            }
        }
        rosbag.close();

        QProcess process;
        QStringList args;
        args << "reindex" << fileName;
        process.start("rosbag" ,args);
    }
}


void DataStreamROS::subscribe()
{
    _subscribers.clear();

    {
        boost::function<void(const rosgraph_msgs::Clock::ConstPtr&) > callback;
        callback = [this](const rosgraph_msgs::Clock::ConstPtr& msg) -> void
        {
            this->clockCallback(msg) ;
        };
        ros::SubscribeOptions ops;
        ops.initByFullCallbackType("/clock", 1, callback );
        ops.transport_hints = ros::TransportHints().tcpNoDelay();
        _clock_subscriber = _node->subscribe(ops);
    }

    for (int i=0; i< _default_topic_names.size(); i++ )
    {
        const std::string topic_name = _default_topic_names[i].toStdString();
        boost::function<void(const topic_tools::ShapeShifter::ConstPtr&) > callback;
        callback = [this, topic_name](const topic_tools::ShapeShifter::ConstPtr& msg) -> void
        {
            this->topicCallback(msg, topic_name) ;
        };

        ros::SubscribeOptions ops;
        ops.initByFullCallbackType(topic_name, 1, callback);
        ops.transport_hints = ros::TransportHints().tcpNoDelay();

        _subscribers.insert( {topic_name, _node->subscribe(ops) }  );
    }
}

bool DataStreamROS::start()
{
    _ros_parser.clear();
    if( !_node )
    {
        _node =  RosManager::getNode();
    }

    if( !_node ){
        return false;
    }

    {
        std::lock_guard<std::mutex> lock( mutex() );
        dataMap().numeric.clear();
        dataMap().user_defined.clear();
    }
    _initial_time = std::numeric_limits<double>::max();

    using namespace RosIntrospection;

    std::vector<std::pair<QString,QString>> all_topics;
    ros::master::V_TopicInfo topic_infos;
    ros::master::getTopics(topic_infos);
    for (ros::master::TopicInfo topic_info: topic_infos)
    {
        all_topics.push_back(
                    std::make_pair(QString(topic_info.name.c_str()),
                                   QString(topic_info.datatype.c_str()) ) );
    }

    QSettings settings;

    if( _default_topic_names.empty())
    {
        // if _default_topic_names is empty (xmlLoad didn't work) use QSettings.
        QVariant def = settings.value("DataStreamROS/default_topics");
        if( !def.isNull() && def.isValid())
        {
            _default_topic_names = def.toStringList();
        }
    }

    QTimer timer;
    timer.setSingleShot(false);
    timer.setInterval( 1000);
    timer.start();

    DialogSelectRosTopics dialog(all_topics, _default_topic_names );

    connect( &timer, &QTimer::timeout, [&]()
    {
        all_topics.clear();
        topic_infos.clear();
        ros::master::getTopics(topic_infos);
        for (ros::master::TopicInfo topic_info: topic_infos)
        {
            all_topics.push_back(
                        std::make_pair(QString(topic_info.name.c_str()),
                                       QString(topic_info.datatype.c_str()) ) );
        }
        dialog.updateTopicList(all_topics);
    });

    int res = dialog.exec();

    timer.stop();

    if( res != QDialog::Accepted || dialog.getSelectedItems().empty() )
    {
        return false;
    }

    _ros_parser.setUseHeaderStamp( dialog.checkBoxTimestamp()->isChecked() );

    if( dialog.checkBoxUseRenamingRules()->isChecked() )
    {
        _ros_parser.addRules( RuleEditing::getRenamingRules() );
    }

    _default_topic_names = dialog.getSelectedItems();

    settings.setValue("DataStreamROS/default_topics", _default_topic_names);

    _ros_parser.setMaxArrayPolicy(dialog.maxArraySize(), dialog.discardEntireArrayIfTooLarge() );

    _prefix = dialog.prefix().toStdString();
    //-------------------------

    subscribe();

    _running = true;

    extractInitialSamples();

    _spinner = std::make_shared<ros::AsyncSpinner>(1);
    _spinner->start();

    _periodic_timer->setInterval(500);
    _roscore_disconnection_already_notified = false;
    _periodic_timer->start();

    return true;
}

bool DataStreamROS::isRunning() const { return _running; }

void DataStreamROS::shutdown()
{
    _periodic_timer->stop();
    if(_spinner)
    {
        _spinner->stop();
    }
    _clock_subscriber.shutdown();
    for(auto& it: _subscribers)
    {
        it.second.shutdown();
    }
    _subscribers.clear();
    _running = false;
    _node.reset();
    _spinner.reset();
}

DataStreamROS::~DataStreamROS()
{
    QSettings settings;
    settings.setValue("DataStreamROS/resetAtLoop", _action_clearBuffer->isChecked() );

    shutdown();
}

QDomElement DataStreamROS::xmlSaveState(QDomDocument &doc) const
{
    QString topics_list = _default_topic_names.join(";");
    QDomElement list_elem = doc.createElement("selected_topics");
    list_elem.setAttribute("list", topics_list );
    return list_elem;
}

bool DataStreamROS::xmlLoadState(QDomElement &parent_element)
{
    QDomElement list_elem = parent_element.firstChildElement( "selected_topics" );
    if( !list_elem.isNull()    )
    {
        if( list_elem.hasAttribute("list") )
        {
            QString topics_list = list_elem.attribute("list");
            _default_topic_names = topics_list.split(";", QString::SkipEmptyParts);
            return true;
        }
    }
    return false;
}

void DataStreamROS::addActionsToParentMenu(QMenu *menu)
{
    _action_saveIntoRosbag = new QAction(QString("Save cached value in a rosbag"), menu);
    QIcon iconSave;
    iconSave.addFile(QStringLiteral(":/icons/resources/light/save.png"), QSize(26, 26));
    _action_saveIntoRosbag->setIcon(iconSave);
    menu->addAction( _action_saveIntoRosbag );

    connect( _action_saveIntoRosbag, &QAction::triggered, this, &DataStreamROS::saveIntoRosbag );

    _action_clearBuffer = new QAction(QString("Clear buffer if Loop restarts"), menu);
    _action_clearBuffer->setCheckable( true );

    QSettings settings;
    bool reset_loop = settings.value("DataStreamROS/resetAtLoop", false).toBool();
    _action_clearBuffer->setChecked( reset_loop );

    menu->addAction( _action_clearBuffer );
}

