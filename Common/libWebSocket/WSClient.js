
  class WebSocketClient {
    constructor(url,getHelloData,onDisconnect){
      this.url = url;
      this.getHelloData = getHelloData;
      this.onDisconnect = onDisconnect;
      this.ws = null;
      this.last_send_ping_time = 0;
      this.last_received_message_time = 0;
      this.intervalId = null;
      this.functionHandlers = {};
      this.eventHandlers = {};
      this.pendingCalls = {};
    }
    intervalHandle(){
      let now = Date.now();
      if(now - this.last_send_ping_time > 30000){
        this.ws.send(JSON.stringify({ type: 'ping' }));
        this.last_send_ping_time = now;
      }
      if(now - this.last_received_message_time > 60000){
        // 超过60秒没有收到消息，认为连接已经断开，触发重连机制
        this.ws.close();
      }
    }
    connect() {
      this.ws = new WebSocket(this.url);
      this.ws.onopen = () => {
        this.last_send_ping_time = Date.now();
        this.last_received_message_time = Date.now();
        let send_root = {};
        send_root.type = 'event';
        send_root.body = {};
        send_root.body.name = 'hello';
        send_root.body.param = this.getHelloData ? this.getHelloData() : {};
        this.ws.send(JSON.stringify(send_root));
        this.intervalId = setInterval(() => this.intervalHandle(), 1000);
      };
      this.ws.onclose = () => {
        clearInterval(this.intervalId);
        this.intervalId = null;
        this.ws = null;
        this.last_send_ping_time = 0;
        this.last_received_message_time = 0;
        if(this.onDisconnect){
          this.onDisconnect();
        }
      };
      this.ws.onerror = (error) => {
        this.ws.close(); // 发生错误时关闭连接，触发重连机制
      };
      this.ws.onmessage = (event) => {
        this.last_received_message_time = Date.now();
        try{
          let message = JSON.parse(event.data);
          switch(message.type){
            case 'ping':{
              this.ws.send(JSON.stringify({ type: 'pong' }));
              break;
            }
            case 'pong':{
              break;
            }
            case 'invoke':{
              // 函数调用请求
              let result_root = {};
              result_root.type = 'response';
              result_root.body = {}
              result_root.body.id = message.body.id;
              let handler = this.functionHandlers[message.body.name];
              if(!handler){
                result_root.body.success = false;
                result_root.body.message = '函数'+message.body.name+'不存在';
                this.ws.send(JSON.stringify(result_root));
                return;
              }
              handler(message.body.param).then(result => {
                result_root.body.success = true;
                result_root.body.data = result;
                this.ws.send(JSON.stringify(result_root));
              }).catch(error => {
                result_root.body.success = false;
                result_root.body.message = error.message;
                this.ws.send(JSON.stringify(result_root));
              });
              break;
            }
            case 'event':{
              // 事件触发
              let handler = this.eventHandlers[message.body.name];
              if(handler){
                handler(message.body.param);
              }
            }
            case 'response':{
              // 响应消息
              let pending = this.pendingCalls[message.body.id];
              if(pending){
                if(message.body.success){
                  pending.resolve(message.body.data);
                }else{
                  pending.reject(new Error(message.body.message));
                }
                delete this.pendingCalls[message.body.id];
              }
              break;
            }
            default:{
              console.warn('未知的消息类型：', message.type);
              break;
            }
          }
        }catch(error){
          console.error('处理消息时发生错误：', error);
        }
      };
    }
    disconnect() {
      if (this.ws) {
        this.ws.close();
        this.ws = null;
      }
    }
    isConnected() {
      return this.ws && this.ws.readyState === WebSocket.OPEN;
    }
    registerFunction(name, handler) {
      this.functionHandlers[name] = handler;
    }
    registerEvent(name, handler) {
      this.eventHandlers[name] = handler;
    }
    callFunction(name, ...args) {
      return new Promise((resolve, reject) => {
        let id = Math.random().toString(36).substr(2, 9);
        this.pendingCalls[id] = { resolve, reject };
        this.ws.send(JSON.stringify({
          type: 'invoke',
          body: {
            id,
            name,
            param: args
          }
        }));
      });
    }
    sendEvent(name, ...args) {
      this.ws.send(JSON.stringify({
        type: 'event',
        body: {
          name,
          param: args
        }
      }));
    }
  }