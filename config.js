// 智能家居网关配置文件
const CONFIG = {
    // 网关 IP 地址和端口
    GATEWAY_HOST: '192.168.1.66',  // 网关默认地址
    GATEWAY_PORT: 80,

    // PC 开发环境
    PC_HOST: '192.168.1.58',

    // 前端生产环境
    FRONTEND_HOST: '192.168.1.250',
    FRONTEND_PORT: 8080,

    // WebSocket 配置
    WEBSOCKET_PATH: '/ws',

    // 动态更新网关地址
    updateGatewayHost: function(newHost) {
        this.GATEWAY_HOST = newHost;
        // 保存到localStorage以便页面刷新后保持
        localStorage.setItem('gateway_host', newHost);
    },

    // 初始化时从localStorage读取网关地址
    init: function() {
        const savedHost = localStorage.getItem('gateway_host');
        if (savedHost) {
            this.GATEWAY_HOST = savedHost;
        }
    },

    // 自动检测环境并返回网关URL
    getGatewayUrl: function() {
        const hostname = window.location.hostname;
        // 在开发PC或生产前端服务器上时，API应指向网关
        if (hostname === this.PC_HOST || 
            hostname === this.FRONTEND_HOST || 
            hostname === 'localhost' || 
            hostname === '127.0.0.1' || 
            window.location.protocol === 'file:') {
            return `http://${this.GATEWAY_HOST}:${this.GATEWAY_PORT}`;
        }
        // 否则，假定UI是从网关本身提供的
        return `${window.location.protocol}//${window.location.host}`;
    },

    getWebSocketUrl: function() {
        const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
        // WebSocket始终连接到网关
        return `${protocol}//${this.GATEWAY_HOST}:${this.GATEWAY_PORT}${this.WEBSOCKET_PATH}`;
    }
};

// 初始化配置
CONFIG.init();

// 导出配置（如果在Node.js环境中）
if (typeof module !== 'undefined' && module.exports) {
    module.exports = CONFIG;
}