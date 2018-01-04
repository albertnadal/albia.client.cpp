#include <string>
#include <vector>
#include <sstream>
#include <iostream>

#include <curlpp/cURLpp.hpp>
#include <curlpp/Easy.hpp>
#include <curlpp/Options.hpp>

#include "rapidjson/document.h"
#include "base64.h"

using namespace rapidjson;
using namespace std;

const vector<string> explode(const string& s, const char& c)
{
   string buff{""};
   vector<string> v;

   for(auto n:s) {
      if(n != c) buff+=n; else
      if(n == c && buff != "") { v.push_back(buff); buff = ""; }
   }
   if(buff != "") v.push_back(buff);

   return v;
}

struct device_token_t {
  string token;
  unsigned int device_id;
};

device_token_t* getDeviceTokenWithAPIKeyAndDeviceKey(const string& host, unsigned int apiPort, const string& apiKey, const string& deviceKey) {

   try {
      curlpp::Cleanup cleaner;
      curlpp::Easy request;
      string scheme("http://");
      string endpoint("/v1/request-device-token");

      curlpp::options::Url serverUrl(scheme+host+endpoint);
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
      const char* json = "{\"project\":\"rapidjson\",\"stars\":10}";
      Document d;
      d.Parse(jsonResponse.c_str());
      Value& s = d["token"];

      device_token_t* deviceToken = new device_token_t;
      deviceToken->token = s.GetString();

      string decodedToken = base64_decode(deviceToken->token);
      vector<string> tokenVector = explode(decodedToken, ';');
      deviceToken->device_id = stoi(tokenVector[0]);

      return deviceToken;

   } catch( curlpp::RuntimeError &e ){
      cout << e.what() << endl;
   } catch( curlpp::LogicError &e ) {
      cout << e.what() << endl;
   }

   return NULL;
}

int main(int, char **)
{
   string hostname("maduixa.lafruitera.com");
   string apiKey("app1234");
   string deviceKey("key1234");

   device_token_t* deviceToken = getDeviceTokenWithAPIKeyAndDeviceKey(hostname, 3001, apiKey, deviceKey);

   if(deviceToken != NULL) {
      cout << "TOKEN: " << deviceToken->token;
      cout << "DEVICE ID: " << deviceToken->device_id;
      delete deviceToken;
   }

  return 0;
}
