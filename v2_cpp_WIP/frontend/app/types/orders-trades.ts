export type DbRecord = {
  record_type: "Order" | "Trade";
  timestamp: string;
  strategy_name: string;
  id: string;
  symbol: string;
  direction: string | null;
  price: number | null;
  volume: number | null;
  traded: number | null;
  status: string | null;
};

export type RecordTypeFilter = "all" | "order" | "trade";
