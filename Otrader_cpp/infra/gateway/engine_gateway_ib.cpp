/**
 * C++ implementation of engines/engine_gateway.py (IbGateway + IbApi).
 * Live 固定使用真实 TWS C++ API，全部实现在本文件。
 */

#include "engine_gateway_ib.hpp"
#include "engine_main.hpp"
#include "ib_mapping.hpp"
#include "../../utilities/constant.hpp"

#include "EPosixClientSocketPlatform.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "DefaultEWrapper.h"
#include "Contract.h"
#include "Order.h"
#include "OrderCancel.h"
#include "OrderState.h"
#include "Execution.h"
#include "Decimal.h"
#include "CommonDefs.h"

#include <ctime>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engines {

// ----- TWS 合约/符号转换（IB Contract <-> 平台 symbol） -----

static std::string contract_to_formatted_symbol(const Contract& c) {
    if (c.symbol.empty()) return c.conId ? std::to_string(c.conId) : "UNKNOWN";
    std::vector<std::string> fields = {c.symbol};
    if (c.secType == "FUT" || c.secType == "OPT" || c.secType == "FOP")
        fields.push_back(c.lastTradeDateOrContractMonth);
    if (c.secType == "OPT" || c.secType == "FOP") {
        fields.push_back(c.right);
        fields.push_back(std::to_string(static_cast<long long>(c.strike)));
        fields.push_back(c.multiplier.empty() ? "100" : c.multiplier);
    }
    fields.push_back(c.currency.empty() ? "USD" : c.currency);
    fields.push_back(c.secType.empty() ? "STK" : c.secType);
    std::string out;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) out += utilities::JOIN_SYMBOL;
        out += fields[i];
    }
    return out;
}

static bool build_ib_single_contract(const std::string& symbol, const std::string* trading_class, Contract& out) {
    out = Contract();
    if (symbol.find(utilities::JOIN_SYMBOL) == std::string::npos) {
        out.symbol = symbol;
        out.secType = "STK";
        out.currency = "USD";
        out.exchange = "SMART";
        if (trading_class && !trading_class->empty()) out.tradingClass = *trading_class;
        return true;
    }
    std::istringstream ss(symbol);
    std::string part;
    std::vector<std::string> fields;
    while (std::getline(ss, part, utilities::JOIN_SYMBOL[0]))
        fields.push_back(part);
    if (fields.size() < 3) return false;
    out.symbol = fields[0];
    out.secType = fields.back();
    out.currency = fields.size() >= 2 ? fields[fields.size() - 2] : "USD";
    if (trading_class && !trading_class->empty()) out.tradingClass = *trading_class;
    out.exchange = "SMART";
    if (out.secType == "OPT" && fields.size() >= 6) {
        out.lastTradeDateOrContractMonth = fields[1];
        out.right = fields[2];
        try {
            out.strike = std::stod(fields[3]);
            out.multiplier = fields[4];
        } catch (...) { return false; }
    }
    return true;
}

static bool build_ib_combo_contract(const std::vector<utilities::Leg>& legs, const std::string* trading_class, Contract& out) {
    out = Contract();
    out.secType = "BAG";
    out.currency = "USD";
    out.exchange = "SMART";
    if (trading_class && !trading_class->empty()) out.tradingClass = *trading_class;
    out.comboLegs = Contract::ComboLegListSPtr(new Contract::ComboLegList);
    for (const auto& leg : legs) {
        auto cl = std::make_shared<ComboLeg>();
        cl->conId = leg.con_id;
        cl->ratio = std::abs(leg.ratio);
        cl->action = (leg.direction == utilities::Direction::LONG) ? "BUY" : "SELL";
        cl->exchange = "SMART";
        out.comboLegs->push_back(cl);
        if (out.symbol.empty() && leg.symbol.has_value() && leg.symbol->find('-') != std::string::npos)
            out.symbol = leg.symbol->substr(0, leg.symbol->find('-'));
        else if (out.symbol.empty() && leg.symbol.has_value())
            out.symbol = *leg.symbol;
    }
    return true;
}

// ----- IbApiTws：EWrapper + EClientSocket，真实 TWS 连接 -----

class IbApiTws : public IbApi, public DefaultEWrapper {
public:
    explicit IbApiTws(IbGateway* gateway)
        : gateway_(gateway)
        , gateway_name_(gateway ? gateway->gateway_name() : "IBGateway")
        , os_signal_(2000)
        , client_(new EClientSocket(this, &os_signal_)) {}

    ~IbApiTws() override {
        if (reader_) reader_.reset();
        if (client_) { client_->eDisconnect(); delete client_; client_ = nullptr; }
    }

    void connect(const std::string& host, int port, int client_id, const std::string& account) override {
        if (status_) return;
        host_ = host;
        port_ = port;
        client_id_ = client_id;
        account_ = account;
        bool ok = client_->eConnect(host.c_str(), port, client_id, false);
        if (!ok) {
            if (gateway_) gateway_->write_log("IB eConnect failed", ERROR);
            return;
        }
        reader_ = std::make_unique<EReader>(client_, &os_signal_);
        reader_->start();
        if (gateway_) gateway_->write_log("IB TWS connecting (wait for nextValidId)...", INFO);
    }

    bool is_connected() const override {
        return status_ && client_ && client_->isConnected();
    }

    void close() override {
        if (!status_ && !client_->isConnected()) return;
        if (reader_) { reader_.reset(); }
        if (client_) client_->eDisconnect();
        clear_account_data();
        status_ = false;
        if (gateway_) gateway_->write_log("IB TWS disconnected", WARNING);
    }

    void check_connection() override {
        if (!client_ || !client_->isConnected()) {
            if (status_) close();
            if (!host_.empty() && gateway_) {
                gateway_->write_log("IB reconnecting...", INFO);
                connect(host_, port_, client_id_, account_);
            }
        }
    }

    void process_pending_messages() override {
        if (reader_) reader_->processMsgs();
    }

    std::string send_order(const utilities::OrderRequest& req) override {
        if (!gateway_ || !client_->isConnected()) return "";
        if (req.type != utilities::OrderType::LIMIT && req.type != utilities::OrderType::MARKET) {
            gateway_->write_log("Unsupported order type", ERROR);
            return "";
        }
        if (req.is_combo && (!req.legs || req.legs->empty())) {
            gateway_->write_log("Combo order requires legs", ERROR);
            return "";
        }
        Contract contract;
        if (req.is_combo)
            build_ib_combo_contract(*req.legs, req.trading_class.has_value() ? &*req.trading_class : nullptr, contract);
        else if (!build_ib_single_contract(req.symbol, req.trading_class.has_value() ? &*req.trading_class : nullptr, contract)) {
            gateway_->write_log("Contract build failed", ERROR);
            return "";
        }
        order_id_++;
        OrderId oid = order_id_;
        Order order;
        order.orderId = oid;
        order.clientId = client_id_;
        order.action = req.is_combo ? "BUY" : direction_vt2ib(req.direction);
        order.orderType = ordertype_vt2ib(req.type);
        order.totalQuantity = DecimalFunctions::stringToDecimal(std::to_string(static_cast<long long>(req.volume)));
        order.account = account_;
        if (req.type == utilities::OrderType::LIMIT)
            order.lmtPrice = req.price;
        std::string orderid = std::to_string(oid);
        utilities::OrderData od = req.create_order_data(orderid, gateway_name_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            orders_[orderid] = od;
            last_order_status_[orderid] = {utilities::Status::SUBMITTING, 0.0};
            pending_orders_.insert(orderid);
        }
        gateway_->on_order(od);
        client_->placeOrder(oid, contract, order);
        return orderid;
    }

    void cancel_order(const utilities::CancelRequest& req) override {
        if (!client_->isConnected()) return;
        OrderCancel oc;
        time_t now = std::time(nullptr);
        struct tm t;
#ifdef _WIN32
        gmtime_s(&t, &now);
#else
        gmtime_r(&now, &t);
#endif
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d:%02d:%02d",
            t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        oc.manualOrderCancelTime = buf;
        client_->cancelOrder(std::stol(req.orderid), oc);
    }

    void query_account() override {
        if (!client_->isConnected()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        req_id_counter_++;
        client_->reqAccountSummary(req_id_counter_, "All", "NetLiquidation,AvailableFunds,MaintMarginReq,UnrealizedPnL");
    }

    void query_position() override {
        if (!client_->isConnected()) return;
        client_->reqPositions();
    }

    void query_portfolio(const std::string& underlying) override {
        if (!client_->isConnected()) return;
        std::vector<std::string> parts;
        std::istringstream ss(underlying);
        std::string p;
        while (std::getline(ss, p, '-')) parts.push_back(p);
        if (parts.size() < 4) return;
        Contract c;
        c.symbol = parts[0];
        c.currency = parts.size() > 1 ? parts[1] : "USD";
        c.secType = parts[2];
        c.exchange = (parts[2] == "IND") ? "CBOE" : parts[3];
        {
            std::lock_guard<std::mutex> lock(mutex_);
            req_id_++;
            client_->reqContractDetails(req_id_, c);
        }
        if (parts[2] == "STK") {
            c.secType = "OPT";
            c.exchange = "SMART";
            std::lock_guard<std::mutex> lock(mutex_);
            req_id_++;
            client_->reqContractDetails(req_id_, c);
            reqid_underlying_map_[req_id_] = underlying;
        } else if (parts[2] == "IND") {
            for (const char* tc : {"SPX", "SPXW"}) {
                Contract opt = c;
                opt.secType = "OPT";
                opt.exchange = "CBOE";
                opt.tradingClass = tc;
                std::lock_guard<std::mutex> lock(mutex_);
                req_id_++;
                client_->reqContractDetails(req_id_, opt);
                reqid_underlying_map_[req_id_] = underlying;
            }
        }
    }

    // --- EWrapper callbacks ---
    void connectAck() override {
        if (!status_) {
            status_ = true;
            if (gateway_) gateway_->write_log("IB TWS connection successful", INFO);
        }
    }

    void connectionClosed() override {
        clear_account_data();
        status_ = false;
        if (gateway_) gateway_->write_log("IB TWS connection closed", WARNING);
    }

    void nextValidId(OrderId orderId) override {
        if (order_id_ <= 0) order_id_ = orderId;
    }

    void managedAccounts(const std::string& accountsList) override {
        if (account_.empty()) {
            std::istringstream ss(accountsList);
            std::string a;
            while (std::getline(ss, a, ','))
                if (!a.empty()) { account_ = a; if (gateway_) gateway_->write_log("Using account: " + account_, INFO); break; }
        }
    }

    void error(int id, time_t, int errorCode, const std::string& errorString, const std::string&) override {
        static const std::unordered_set<int> harmless = {202, 2104, 2106, 2158};
        if (harmless.count(errorCode)) return;
        if (gateway_) gateway_->write_log("IB Error [" + std::to_string(errorCode) + "]: " + errorString, ERROR);
    }

    void orderStatus(OrderId orderId, const std::string& status, Decimal filled, Decimal remaining,
                     double avgFillPrice, long long, int, double, int, const std::string&, double) override {
        std::string orderid = std::to_string(orderId);
        utilities::Status st = status_ib2vt(status);
        double fill = DecimalFunctions::decimalToDouble(filled);
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = orders_.find(orderid);
        if (it == orders_.end()) return;
        auto& last = last_order_status_[orderid];
        if (std::make_pair(st, fill) == last) return;
        it->second.traded = fill;
        it->second.status = st;
        last = {st, fill};
        gateway_->on_order(it->second);
        if (st == utilities::Status::ALLTRADED || st == utilities::Status::CANCELLED || st == utilities::Status::REJECTED) {
            last_order_status_.erase(orderid);
            orders_.erase(it);
            completed_orders_.insert(orderid);
        }
    }

    void openOrder(OrderId orderId, const Contract& contract, const Order& order, const OrderState&) override {
        std::string orderid = std::to_string(orderId);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_orders_.count(orderid)) { pending_orders_.erase(orderid); return; }
            if (completed_orders_.count(orderid) || orders_.count(orderid)) return;
        }
        utilities::OrderData od;
        od.gateway_name = gateway_name_;
        od.symbol = contract_to_formatted_symbol(contract);
        od.exchange = utilities::Exchange::SMART;
        od.orderid = orderid;
        od.type = ordertype_ib2vt(order.orderType);
        od.direction = direction_ib2vt(order.action);
        od.volume = DecimalFunctions::decimalToDouble(order.totalQuantity);
        od.price = (order.orderType == "LMT") ? order.lmtPrice : 0;
        od.status = utilities::Status::SUBMITTING;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            orders_[orderid] = od;
            last_order_status_[orderid] = {utilities::Status::SUBMITTING, 0.0};
        }
        gateway_->on_order(od);
    }

    void execDetails(int, const Contract& contract, const Execution& execution) override {
        std::string orderid = std::to_string(execution.orderId);
        std::string symbol;
        utilities::Direction dir = direction_ib2vt(execution.side);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = orders_.find(orderid);
            if (it != orders_.end()) {
                symbol = it->second.symbol;
                if (it->second.is_combo && it->second.direction.has_value())
                    dir = it->second.direction.value();
            } else {
                symbol = contract_to_formatted_symbol(contract);
            }
        }
        utilities::TradeData td;
        td.gateway_name = gateway_name_;
        td.symbol = symbol;
        td.exchange = utilities::Exchange::SMART;
        td.orderid = orderid;
        td.tradeid = execution.execId;
        td.direction = dir;
        td.price = execution.price;
        td.volume = DecimalFunctions::decimalToDouble(execution.shares);
        gateway_->on_trade(td);
    }

    void accountSummary(int reqId, const std::string& account, const std::string& tag, const std::string& value, const std::string&) override {
        (void)reqId;
        if (tag != "NetLiquidation" && tag != "AvailableFunds" && tag != "MaintMarginReq" && tag != "UnrealizedPnL") return;
        std::lock_guard<std::mutex> lock(mutex_);
        account_values_[account][tag] = value;
    }

    void accountSummaryEnd(int reqId) override {
        (void)reqId;
        account_values_.clear();
    }

    void position(const std::string&, const Contract& contract, Decimal position, double avgCost) override {
        (void)contract;
        (void)position;
        (void)avgCost;
    }

    void positionEnd() override {}

    void contractDetails(int reqId, const ContractDetails& details) override {
        const Contract& c = details.contract;
        if (product_ib2vt(c.secType) == utilities::Product::UNKNOWN) return;
        utilities::Exchange exch = exchange_ib2vt(c.exchange.empty() ? c.primaryExchange : c.exchange);
        utilities::ContractData cd;
        cd.gateway_name = gateway_name_;
        cd.symbol = contract_to_formatted_symbol(c);
        cd.exchange = exch;
        cd.name = details.longName;
        cd.product = product_ib2vt(c.secType);
        cd.size = c.multiplier.empty() ? 1.0 : std::stod(c.multiplier);
        cd.pricetick = details.minTick;
        cd.min_volume = (details.minSize == UNSET_DECIMAL) ? 1.0 : DecimalFunctions::decimalToDouble(details.minSize);
        cd.net_position = true;
        cd.history_data = true;
        cd.stop_supported = true;
        cd.con_id = c.conId;
        if (!c.tradingClass.empty()) cd.trading_class = c.tradingClass;
        if (cd.product == utilities::Product::OPTION) {
            cd.option_type = option_ib2vt(c.right);
            cd.option_strike = c.strike;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (contracts_.count(cd.symbol) == 0) {
            gateway_->on_contract(cd);
            contracts_[cd.symbol] = cd;
        }
    }

    void contractDetailsEnd(int reqId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = reqid_underlying_map_.find(reqId);
        if (it != reqid_underlying_map_.end() && gateway_)
            gateway_->write_log("Option portfolio query complete: " + it->second, INFO);
    }

private:
    void clear_account_data() {
        std::lock_guard<std::mutex> lock(mutex_);
        account_values_.clear();
    }

    IbGateway* gateway_ = nullptr;
    std::string gateway_name_;
    bool status_ = false;
    int order_id_ = 0;
    int req_id_ = 0;
    int req_id_counter_ = 9000;
    int client_id_ = 0;
    std::string account_;
    std::string host_;
    int port_ = 7497;

    EReaderOSSignal os_signal_;
    EClientSocket* client_ = nullptr;
    std::unique_ptr<EReader> reader_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, utilities::OrderData> orders_;
    std::unordered_map<std::string, std::pair<utilities::Status, double>> last_order_status_;
    std::unordered_set<std::string> pending_orders_;
    std::unordered_set<std::string> completed_orders_;
    std::unordered_map<std::string, utilities::ContractData> contracts_;
    std::unordered_map<int, std::string> reqid_underlying_map_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> account_values_;
};

// ----- IbGateway 实现 -----

IbGateway::IbGateway(MainEngine* main_engine)
    : main_engine_(main_engine) {
    api_ = std::make_unique<IbApiTws>(this);
}

IbGateway::~IbGateway() = default;

void IbGateway::write_log(const std::string& msg, int level) {
    if (main_engine_) main_engine_->write_log(msg, level, gateway_name_);
}

void IbGateway::on_order(const utilities::OrderData& order) {
    if (main_engine_) {
        main_engine_->put_event(utilities::Event(utilities::EventType::Order, order));
    }
}

void IbGateway::on_trade(const utilities::TradeData& trade) {
    if (main_engine_) {
        main_engine_->put_event(utilities::Event(utilities::EventType::Trade, trade));
    }
}

void IbGateway::on_contract(const utilities::ContractData& contract) {
    if (main_engine_) {
        main_engine_->put_event(utilities::Event(utilities::EventType::Contract, contract));
    }
}

void IbGateway::connect() {
    api_->connect(
        default_setting_.host,
        default_setting_.port,
        default_setting_.client_id,
        default_setting_.account);
}

void IbGateway::disconnect() {
    api_->close();
}

bool IbGateway::is_connected() const {
    return api_ && api_->is_connected();
}

std::string IbGateway::send_order(const utilities::OrderRequest& req) {
    return api_->send_order(req);
}

void IbGateway::cancel_order(const utilities::CancelRequest& req) {
    api_->cancel_order(req);
}

void IbGateway::query_account() {
    api_->query_account();
}

void IbGateway::query_position() {
    api_->query_position();
}

void IbGateway::query_portfolio(const std::string& underlying) {
    api_->query_portfolio(underlying);
}

void IbGateway::process_timer_event(const utilities::Event&) {
    if (api_) api_->process_pending_messages();
    count_++;
    if (count_ < 10) return;
    count_ = 0;
    api_->check_connection();
}

}  // namespace engines
