#include <unistd.h>

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <curlpp/cURLpp.hpp>
#include <curlpp/Easy.hpp>
#include <curlpp/Options.hpp>
#include <sio_client.h>
#include <SQLiteCpp/SQLiteCpp.h>
#include <SQLiteCpp/VariadicBind.h>
#include "rapidjson/document.h"
#include "rxcpp/rx.hpp"
#include "base64.h"
#include "proto3/albia.pb.h"

using namespace sio;
using namespace rapidjson;
using namespace std;

std::mutex _lock;
std::condition_variable_any _cond;

class SIOConnectionListener
{
    sio::client *handler;

public:

    bool connectFinished;

    SIOConnectionListener(sio::client* h):
    handler(h)
    {
      this->connectFinished = false;
    }

    void on_connected()
    {
        _lock.lock();
        _cond.notify_all();
        this->connectFinished = true;
        _lock.unlock();
    }

    void on_close(client::close_reason const& reason)
    {
        exit(0);
    }

    void on_fail()
    {
        exit(0);
    }
};

class DeviceTimestamp {

public:
uint64_t unixTimestamp;
uint64_t microseconds;

DeviceTimestamp();
DeviceTimestamp(uint64_t _unix_timestamp);
DeviceTimestamp(uint64_t _unix_timestamp, uint64_t _microseconds);
};

DeviceTimestamp::DeviceTimestamp() {
  const auto now     = std::chrono::system_clock::now();
  const auto epoch   = now.time_since_epoch();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(epoch);
  this->unixTimestamp = seconds.count();
  this->microseconds = 0;
}

DeviceTimestamp::DeviceTimestamp(uint64_t _unix_timestamp) {
  this->unixTimestamp = _unix_timestamp;
  this->microseconds = 0;
}

DeviceTimestamp::DeviceTimestamp(uint64_t _unix_timestamp, uint64_t _microseconds) {
  this->unixTimestamp = _unix_timestamp;
  this->microseconds = _microseconds;
}

class DeviceClient {

public:

struct device_token_t {
        string token;
        unsigned int device_id;
};

bool isConnected;
DeviceClient(const string& apiKey, const string& deviceKey, const string& hostname);
void connect(const string& hostname);
void reconnect();
void disconnect();
void onConnect(const std::function<void()>& callback);
void onConnectError(const std::function<void(const std::exception& ex)>& callback);
void onDisconnect(const std::function<void()>& callback);
void writeDataBool(const string& key, bool data, DeviceTimestamp* timestamp);
void writeDataInt32(const string& key, int32_t data, DeviceTimestamp* timestamp);
void writeDataInt64(const string& key, int64_t data, DeviceTimestamp* timestamp);
void writeDataUInt32(const string& key, uint32_t data, DeviceTimestamp* timestamp);
void writeDataUInt64(const string& key, uint64_t data, DeviceTimestamp* timestamp);
void writeDataDouble(const string& key, double data, DeviceTimestamp* timestamp);
void writeDataFloat(const string& key, float data, DeviceTimestamp* timestamp);
void writeDataString(const string& key, const string& data, DeviceTimestamp* timestamp);

private:

string apiKey;
string deviceKey;
string host;
unsigned int apiPort;
unsigned int webSocketPort;
string dbFilename;
string dbFolder;
string lastRecordFilename;
string deviceToken;
unsigned int deviceId;
string socketIOnamespace;
sio::client *socketIO;
sio::socket::ptr currentSocket;
SIOConnectionListener *connectionListener;
SQLite::Database *db;
SQLite::Database *dbThread;
std::thread *writeQueueThread;
bool writeQueueThreadIsRunning;
static const vector<string> explode(const string& s, const char& c);
std::function<void()> onConnectCallback;
std::function<void(const std::exception& ex)> onConnectErrorCallback;
std::function<void()> onDisconnectCallback;
device_token_t* getDeviceTokenWithAPIKeyAndDeviceKey(const string& hostname, unsigned int apiPort, const string& apiKey, const string& deviceKey);
rxcpp::observable<int> connectToServer(const string& hostname, unsigned int apiPort, unsigned int webSocketPort, const string& deviceToken, const string& apiKey, const string& deviceKey);
void startWriteQueueThread();
void enqueueWriteOperation(DeviceRecordMsg *record);
unsigned int getLastRecordIdSent();
void setLastRecordIdSent(unsigned int value);
google::protobuf::Timestamp* getProtobufTimestampFromDeviceTimestamp(DeviceTimestamp* timestamp);
std::string getWorkingFolder();
bool fileExists(const std::string& name);
};

DeviceClient::DeviceClient(const string& apiKey, const string& deviceKey, const string& hostname) {
        this->isConnected = false;
        this->apiKey = apiKey;
        this->deviceKey = deviceKey;
        this->host = hostname;
        this->apiPort = 3001;
        this->webSocketPort = 3000;
        this->deviceId = 0;
        this->dbFilename = "albia.sqlite";
        this->lastRecordFilename = "albia.lastid";
        this->deviceToken = "";
        this->socketIOnamespace = "";
        this->onConnectCallback = NULL;
        this->onConnectErrorCallback = NULL;
        this->socketIO = NULL;
        this->connectionListener = NULL;
        this->currentSocket = NULL;
        this->writeQueueThreadIsRunning = false;
        this->dbFolder = this->getWorkingFolder();

        if(!this->fileExists(this->dbFolder+"/"+this->dbFilename)) {
          this->dbThread = new SQLite::Database(this->dbFolder+"/"+this->dbFilename, SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE);
          this->dbThread->exec("CREATE TABLE write_operation (id_write_operation INTEGER PRIMARY KEY AUTOINCREMENT, id_device INTEGER NOT NULL, timestamp INTEGER NOT NULL, payload BLOB NOT NULL, sending INTEGER DEFAULT 0)");
          this->db = new SQLite::Database(this->dbFolder+"/"+this->dbFilename, SQLite::OPEN_READWRITE);
        } else {
          this->dbThread = new SQLite::Database(this->dbFolder+"/"+this->dbFilename, SQLite::OPEN_READWRITE);
          this->db = new SQLite::Database(this->dbFolder+"/"+this->dbFilename, SQLite::OPEN_READWRITE);
        }

        this->dbThread->exec("PRAGMA journal_mode = wal;");
        this->dbThread->exec("PRAGMA auto_vacuum = FULL;");
        this->dbThread->exec("vacuum");
}

bool DeviceClient::fileExists(const std::string& name) {
  return ( access( name.c_str(), F_OK ) != -1 );
}

std::string DeviceClient::getWorkingFolder() {
  char cwd[1024];
  getcwd(cwd, sizeof(cwd));
  std::string folder(cwd);
  return folder;
}

const vector<string> DeviceClient::explode(const string& s, const char& c) {
        string buff{""};
        vector<string> v;

        for(auto n:s) {
                if(n != c) buff+=n; else
                if(n == c && buff != "") { v.push_back(buff); buff = ""; }
        }
        if(buff != "") v.push_back(buff);

        return v;
}

void DeviceClient::onConnect(const std::function<void()>& callback) {
   this->onConnectCallback = callback;
}

void DeviceClient::onConnectError(const std::function<void(const std::exception&)>& callback) {
   this->onConnectErrorCallback = callback;
}

void DeviceClient::onDisconnect(const std::function<void()>& callback) {
   this->onDisconnectCallback = callback;
}

google::protobuf::Timestamp* DeviceClient::getProtobufTimestampFromDeviceTimestamp(DeviceTimestamp* timestamp) {
  google::protobuf::Timestamp *utcDate = new google::protobuf::Timestamp();
  if (timestamp == NULL) {
      std::chrono::nanoseconds ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch());
      std::chrono::seconds s = std::chrono::duration_cast<std::chrono::seconds>(ns);
      ns -= s;
      uint64_t seconds = s.count();
      uint64_t nanoseconds = ns.count();
      utcDate->set_seconds(seconds);
      utcDate->set_nanos(nanoseconds);
  } else {
      utcDate->set_seconds(timestamp->unixTimestamp);
      utcDate->set_nanos(timestamp->microseconds * 1000); // 1 microsecond = 1000 nanoseconds
  }
  return utcDate;
}

unsigned int DeviceClient::getLastRecordIdSent() {
  if (!this->fileExists(this->dbFolder+"/"+this->lastRecordFilename)) {
      return 0;
  } else {
      std::ifstream ifs(this->dbFolder+"/"+this->lastRecordFilename);
      std::string lastId((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));

      if(lastId == "") {
          return 0;
      } else {
          return atoi(lastId.c_str());
      }
  }
}

void DeviceClient::setLastRecordIdSent(unsigned int value) {
  std::ofstream ofs;
  ofs.open(this->dbFolder+"/"+this->lastRecordFilename, ios::out);
  ofs << std::to_string(value);
  ofs.close();
}

void DeviceClient::startWriteQueueThread() {

  if(this->writeQueueThreadIsRunning) {
    return;
  }

  this->writeQueueThreadIsRunning = true;
  this->writeQueueThread = new std::thread([&]() {

    bool success = true;
    bool empty = false;
    unsigned int lastRecordIdSent = 0;

    while((this->isConnected) && (success)) {

      empty = false;
      while((this->isConnected) && (!empty)) {

        lastRecordIdSent = this->getLastRecordIdSent();
        SQLite::Statement query(*this->dbThread, "SELECT id_write_operation AS id_write_operation, payload AS payload, timestamp AS timestamp FROM write_operation WHERE timestamp = (SELECT MIN(timestamp) FROM write_operation WHERE id_write_operation > "+std::to_string(lastRecordIdSent)+" AND sending = 0) AND id_write_operation > "+std::to_string(lastRecordIdSent)+" AND sending = 0 ORDER BY id_device ASC LIMIT 1");
        if(query.executeStep()) {
          unsigned int id_write_operation = query.getColumn(0).getInt();
          const void* payload_blob = query.getColumn(1).getBlob();
          size_t payload_size = query.getColumn(1).getBytes();
          unsigned int timestamp = query.getColumn(2).getInt();
cout << "ID: " << std::to_string(id_write_operation) << endl;
cout << "PAYLOAD SIZE: " << std::to_string(payload_size) << endl;
cout << "UNIX TIMESTAMP: " << std::to_string(timestamp) << endl;
          DeviceRecordMsg *payload = new DeviceRecordMsg();
          payload->ParseFromArray(payload_blob, payload_size);
          payload->set_deviceid(this->deviceId);
          google::protobuf::Timestamp *utcDate = new google::protobuf::Timestamp();
          utcDate->set_seconds(timestamp);
          utcDate->set_nanos(0);
          payload->set_allocated_date(utcDate);

          if(this->isConnected) {
cout << "SENDING MSG" << endl;
            std::string buffer_string;
            payload->SerializeToString(&buffer_string);
            this->currentSocket->emit("write", std::make_shared<std::string>(buffer_string.c_str(), buffer_string.length()));
            this->setLastRecordIdSent(id_write_operation);
          }

        } else {
          empty = true;
        }

      }
    }

    this->writeQueueThreadIsRunning = false;
  });
}

void DeviceClient::enqueueWriteOperation(DeviceRecordMsg *record) {
  size_t size = record->ByteSizeLong();
  void *buffer = malloc(size);
  record->SerializeToArray(buffer, size);

  SQLite::Statement query(*this->db, "INSERT INTO write_operation (id_device, timestamp, payload, sending) VALUES (0, "+std::to_string(record->date().seconds())+", ?, 0)");
  query.bind(1, buffer, size);
  query.exec();

  free(buffer);
  delete record;

  if(this->isConnected) {
    this->startWriteQueueThread();
  }

  unsigned int lastRecordIdSent = this->getLastRecordIdSent();
  this->db->exec("DELETE FROM write_operation WHERE id_write_operation <= "+std::to_string(lastRecordIdSent));
}

void DeviceClient::writeDataBool(const string& key, bool data, DeviceTimestamp* timestamp = NULL) {
  DeviceRecordMsg *record = new DeviceRecordMsg();
  record->set_deviceid(0);
  record->set_key(key);
  record->set_boolvalue(data);
  record->set_type(DeviceRecordMsg_RecordType_BOOL);
  google::protobuf::Timestamp *utcDate = this->getProtobufTimestampFromDeviceTimestamp(timestamp);
  record->set_allocated_date(utcDate);
  this->enqueueWriteOperation(record);
}

void DeviceClient::writeDataInt32(const string& key, int32_t data, DeviceTimestamp* timestamp = NULL) {
  DeviceRecordMsg *record = new DeviceRecordMsg();
  record->set_deviceid(0);
  record->set_key(key);
  record->set_int32value(data);
  record->set_type(DeviceRecordMsg_RecordType_INT32);
  google::protobuf::Timestamp *utcDate = this->getProtobufTimestampFromDeviceTimestamp(timestamp);
  record->set_allocated_date(utcDate);
  this->enqueueWriteOperation(record);
}

void DeviceClient::writeDataInt64(const string& key, int64_t data, DeviceTimestamp* timestamp = NULL) {
  DeviceRecordMsg *record = new DeviceRecordMsg();
  record->set_deviceid(0);
  record->set_key(key);
  record->set_int64value(data);
  record->set_type(DeviceRecordMsg_RecordType_INT64);
  google::protobuf::Timestamp *utcDate = this->getProtobufTimestampFromDeviceTimestamp(timestamp);
  record->set_allocated_date(utcDate);
  this->enqueueWriteOperation(record);
}

void DeviceClient::writeDataUInt32(const string& key, uint32_t data, DeviceTimestamp* timestamp = NULL) {
  DeviceRecordMsg *record = new DeviceRecordMsg();
  record->set_deviceid(0);
  record->set_key(key);
  record->set_uint32value(data);
  record->set_type(DeviceRecordMsg_RecordType_UINT32);
  google::protobuf::Timestamp *utcDate = this->getProtobufTimestampFromDeviceTimestamp(timestamp);
  record->set_allocated_date(utcDate);
  this->enqueueWriteOperation(record);
}

void DeviceClient::writeDataUInt64(const string& key, uint64_t data, DeviceTimestamp* timestamp = NULL) {
  DeviceRecordMsg *record = new DeviceRecordMsg();
  record->set_deviceid(0);
  record->set_key(key);
  record->set_uint64value(data);
  record->set_type(DeviceRecordMsg_RecordType_UINT64);
  google::protobuf::Timestamp *utcDate = this->getProtobufTimestampFromDeviceTimestamp(timestamp);
  record->set_allocated_date(utcDate);
  this->enqueueWriteOperation(record);
}

void DeviceClient::writeDataDouble(const string& key, double data, DeviceTimestamp* timestamp = NULL) {
  DeviceRecordMsg *record = new DeviceRecordMsg();
  record->set_deviceid(0);
  record->set_key(key);
  record->set_doublevalue(data);
  record->set_type(DeviceRecordMsg_RecordType_DOUBLE);
  google::protobuf::Timestamp *utcDate = this->getProtobufTimestampFromDeviceTimestamp(timestamp);
  record->set_allocated_date(utcDate);
  this->enqueueWriteOperation(record);
}

void DeviceClient::writeDataFloat(const string& key, float data, DeviceTimestamp* timestamp = NULL) {
  DeviceRecordMsg *record = new DeviceRecordMsg();
  record->set_deviceid(0);
  record->set_key(key);
  record->set_floatvalue(data);
  record->set_type(DeviceRecordMsg_RecordType_FLOAT);
  google::protobuf::Timestamp *utcDate = this->getProtobufTimestampFromDeviceTimestamp(timestamp);
  record->set_allocated_date(utcDate);
  this->enqueueWriteOperation(record);
}

void DeviceClient::writeDataString(const string& key, const string& data, DeviceTimestamp* timestamp = NULL) {
  DeviceRecordMsg *record = new DeviceRecordMsg();
  record->set_deviceid(0);
  record->set_key(key);
  record->set_stringvalue(data);
  record->set_type(DeviceRecordMsg_RecordType_STRING);
  google::protobuf::Timestamp *utcDate = this->getProtobufTimestampFromDeviceTimestamp(timestamp);
  record->set_allocated_date(utcDate);
  this->enqueueWriteOperation(record);
}

DeviceClient::device_token_t* DeviceClient::getDeviceTokenWithAPIKeyAndDeviceKey(const string& hostname, unsigned int apiPort, const string& apiKey, const string& deviceKey) {

        try {
                curlpp::Cleanup cleaner;
                curlpp::Easy request;
                string scheme("http://");
                string endpoint("/v1/request-device-token");

                curlpp::options::Url serverUrl(scheme+hostname+endpoint);
                curlpp::options::Port serverPort(apiPort);

                list<string> headersList;
                headersList.push_back("Accept: application/json");
                headersList.push_back("X-albia-api-key: "+apiKey);
                headersList.push_back("X-albia-device-key: "+deviceKey);
                curlpp::options::HttpHeader headers(headersList);

                ostringstream os;
                curlpp::options::WriteStream ws(&os);
                request.setOpt(ws);
                request.setOpt(headers);
                request.setOpt(serverUrl);
                request.setOpt(serverPort);
                request.perform();

                string jsonResponse = os.str();
                cout << "RESPONSE: " << jsonResponse << endl;
                Document d;
                d.Parse(jsonResponse.c_str());
                Value& s = d["token"];

                device_token_t* deviceToken = new device_token_t;
                deviceToken->token = s.GetString();

                string decodedToken = base64_decode(deviceToken->token);
                vector<string> tokenVector = DeviceClient::explode(decodedToken, ';');
                deviceToken->device_id = stoi(tokenVector[0]);

                return deviceToken;

        } catch( curlpp::RuntimeError &e ) {
                cout << e.what() << endl;
        } catch( curlpp::LogicError &e ) {
                cout << e.what() << endl;
        }

        return NULL;
}

void DeviceClient::connect(const string& hostname = "") {

  if(this->isConnected) {
    return;
  }

  if(hostname != "") {
    this->host = hostname;
  }

  rxcpp::observable<int> observable = this->connectToServer(this->host, this->apiPort, this->webSocketPort, this->deviceToken, this->apiKey, this->deviceKey);

  observable.subscribe(
          [](int v) {
  },
          [this](std::exception_ptr ep){
          this->isConnected = false;
          try {std::rethrow_exception(ep);}
          catch (const std::exception& ex) {
                  if(this->onConnectErrorCallback != NULL) {
                    (this->onConnectErrorCallback)(ex);
                  }
          }
  },
          [this](){

            if(this->socketIO != NULL) {
              delete this->socketIO;
            }

            if(this->connectionListener != NULL) {
              delete this->connectionListener;
            }

            if(this->currentSocket != NULL) {
              delete &this->currentSocket;
            }

            cout << endl << "Creant instancia de sio::client" << endl;
            this->socketIO = new sio::client();
            cout << endl << "Creant instancia de SIOConnectionListener" << endl;
            this->connectionListener = new SIOConnectionListener(this->socketIO);

            this->socketIO->set_open_listener(std::bind(&SIOConnectionListener::on_connected, this->connectionListener));
            this->socketIO->set_close_listener(std::bind(&SIOConnectionListener::on_close, this->connectionListener,std::placeholders::_1));
            this->socketIO->set_fail_listener(std::bind(&SIOConnectionListener::on_fail, this->connectionListener));

            cout << endl << "Connectant websocket... " << "http://"+this->host+":"+std::to_string(this->webSocketPort) << endl;
            std::map<std::string, std::string> httpHeaders;
            httpHeaders["authorization"] = this->deviceToken;
            this->socketIO->connect("http://"+this->host+":"+std::to_string(this->webSocketPort), {}, httpHeaders);

            _lock.lock();
            if(!this->connectionListener->connectFinished)
            {
                _cond.wait(_lock);
            }
            _lock.unlock();

            this->currentSocket = this->socketIO->socket("/v1/"+this->socketIOnamespace);
            cout << "Current namespace: " << this->currentSocket->get_namespace() << endl;

/*
            // Delete old records sent
            $lastRecordIdSent = $self->getLastRecordIdSent();
            $self->db->exec("DELETE FROM write_operation WHERE id_write_operation <= $lastRecordIdSent");
*/

            cout << "DEVICE ID: " << this->deviceId << " DEVICE TOKEN: " << this->deviceToken << endl << "NAMESPACE: " << this->socketIOnamespace << endl;
            this->isConnected = true;
/*
            $this->startWriteQueueThread();
            $this->startHeartBeatTimer();
*/
            if(this->onConnectCallback != NULL) {
              (this->onConnectCallback)();
            }
/*
            // Launch the event loop of the socketIO
            $this->runLoop();
*/
  });

}

void DeviceClient::reconnect() {
   this->connect(this->host);
}

void DeviceClient::disconnect() {
   if(this->isConnected) {
     this->currentSocket->off_all();
     this->currentSocket->off_error();
     this->currentSocket->close();
     this->socketIO->sync_close();
     this->socketIO->clear_con_listeners();
     this->isConnected = false;

     if(this->onDisconnectCallback != NULL) {
       (this->onDisconnectCallback)();
     }
   }
}

rxcpp::observable<int> DeviceClient::connectToServer(const string& hostname, unsigned int apiPort, unsigned int webSocketPort, const string& deviceToken, const string& apiKey, const string& deviceKey) {

        static rxcpp::observable<int> observable = rxcpp::observable<>::create<int>(
                [this, &deviceToken, &hostname, apiPort, &apiKey, &deviceKey](rxcpp::subscriber<int> observer){
                try {
                        cout << "Connecting to server..." << endl;
                        string localDeviceToken("");

                        if(deviceToken.empty()) {
                                device_token_t* deviceTokenData = this->getDeviceTokenWithAPIKeyAndDeviceKey(hostname, 3001, apiKey, deviceKey);
                                if(deviceTokenData != NULL) {
                                        localDeviceToken = deviceTokenData->token;
                                        this->deviceId = deviceTokenData->device_id;
                                }
                        } else {
                                localDeviceToken = deviceToken;
                        }

                        this->deviceToken = localDeviceToken;
                        curlpp::Cleanup cleaner;
                        curlpp::Easy request;
                        string scheme("http://");
                        string endpoint("/v1/request-namespace");

                        curlpp::options::Url serverUrl(scheme+hostname+endpoint);
                        curlpp::options::Port serverPort(apiPort);

                        list<string> headersList;
                        headersList.push_back("Accept: application/json");
                        headersList.push_back("Authorization: "+localDeviceToken);
                        curlpp::options::HttpHeader headers(headersList);

                        ostringstream os;
                        curlpp::options::WriteStream ws(&os);
                        request.setOpt(ws);
                        request.setOpt(headers);
                        request.setOpt(serverUrl);
                        request.setOpt(serverPort);
                        request.perform();

                        string jsonResponse = os.str();
                        cout << "RESPONSE: " << jsonResponse << endl;
                        Document d;
                        d.Parse(jsonResponse.c_str());
                        Value& s = d["namespace"];
                        this->socketIOnamespace = s.GetString();

                        observer.on_completed();
                } catch( curlpp::RuntimeError &e ) {
                        cout << e.what() << endl;
                        observer.on_error(std::current_exception());
                } catch( curlpp::LogicError &e ) {
                        cout << e.what() << endl;
                        observer.on_error(std::current_exception());
                }

        });

        return observable;
}

int main(int, char **)
{
        string hostname("localhost"); //"maduixa.lafruitera.com");
        string apiKey("app1234");
        string deviceKey("key1234");

        DeviceClient *client = new DeviceClient(apiKey, deviceKey, hostname);

        client->onConnect([&client]() {
          cout << "Connected!" << endl;
          client->writeDataBool("clau", true);
usleep(3000000);
          client->writeDataInt32("clau", 652356);
usleep(3000000);
          client->writeDataInt64("clau", 4524345321);
usleep(3000000);
          client->writeDataUInt32("clau", 23324324);
usleep(3000000);
          client->writeDataUInt64("clau", 2332432432324);
usleep(3000000);
          client->writeDataDouble("clau", 34332332432432324);
usleep(3000000);
          client->writeDataFloat("clau", 342.432432324);
usleep(3000000);
          client->writeDataString("clau", std::string("hello world"));
usleep(3000000);

          client->disconnect();
        });

        client->onDisconnect([]() {
          cout << "Disconnected!" << endl;
        });

        client->onConnectError([](const std::exception& ex) {
          cout << "ERROR when connecting: " << ex.what() << endl;
        });

        client->connect();
usleep(20000000);
        return 0;
}
