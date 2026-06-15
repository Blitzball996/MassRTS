#pragma once
// ============================================================================
// UNIT BALANCE CONFIG - 修改这些数值来调整游戏平衡
// 直接改数字即可，重新编译后生效
// ============================================================================

// 经济
constexpr int STARTING_MONEY = 30000;       // 初始金币
constexpr int PASSIVE_INCOME = 200;         // 每秒被动收入
constexpr int KILL_BOUNTY_BASE = 10;        // 击杀基础赏金
constexpr float CAPTURE_INCOME_MULT = 1.5f; // 占领据点后收入倍率
constexpr int NUKE_COST = 50000;            // 核弹解锁费用

// 胜利条件
constexpr float VICTORY_HOLD_TIME = 30.0f;  // 需要占领多少秒获胜
constexpr int VICTORY_POINTS_NEEDED = 3;    // 需要占领几个据点(共5个)

// 部队生成
constexpr int DEFAULT_BUY_COUNT = 1000;     // 默认批量购买数
constexpr int MAX_BUY_COUNT = 10000;        // 最大单次购买
constexpr int MIN_BUY_COUNT = 100;          // 最小单次购买

// AI敌军
constexpr float AI_BUDGET_INTERVAL = 5.0f;  // AI多久买一次兵(秒)
constexpr int AI_BUDGET_PER_WAVE = 10000;   // AI每波预算
constexpr float AI_AGGRESSION = 1.0f;       // AI攻击性(0.5=保守, 2.0=激进)

// 战斗系统
constexpr float COMBAT_COOLDOWN = 1.0f;     // 攻击冷却(秒)
constexpr float SEPARATION_RADIUS = 4.0f;   // 单位分离半径
constexpr float SEPARATION_FORCE = 8.0f;    // 分离力度

// ============================================================================
// UNIT SHOP DEFINITIONS - 兵种商店数据
// {name, type, cost, hp, damage, range, speed, scale}
// ============================================================================
struct UnitShopEntry {
    const char* name;
    int type_id;
    int cost;
    float hp;
    float damage;
    float range;
    float speed;
    float scale;
};

constexpr int SHOP_COUNT = 9;
static const UnitShopEntry UNIT_SHOP_CONFIG[SHOP_COUNT] = {
    // name        type  cost   hp    dmg   range  spd   scale
    {"Militia",      7,   50,   60,    6,    6,    5.0f,  2.0f},
    {"Infantry",     0,  100,  100,   10,    8,    6.0f,  2.0f},
    {"Cavalry",      1,  200,  150,   18,    8,   12.0f,  2.5f},
    {"Archer",       2,  150,   60,   12,  100,    4.0f,  1.8f},
    {"Bomber",       3,  300,   80,   80,   12,    5.0f,  2.2f},
    {"Artillery",    4,  500,   70,   40,  200,    2.0f,  2.8f},
    {"Shield",       5,  250,  200,    8,    6,    4.0f,  2.2f},
    {"Samurai",      6,  350,  120,   22,    8,   10.0f,  2.3f},
    {"Wall",         8,  100,  300,    0,    0,    0.0f,  3.0f},
};

// ============================================================================
// TERRAIN MODIFIERS - 地形对速度的影响
// ============================================================================
constexpr float SPEED_ON_WATER = 0.35f;     // 水中速度倍率
constexpr float SPEED_ON_SWAMP = 0.70f;     // 沼泽速度倍率
constexpr float SPEED_ON_ROAD = 1.30f;      // 道路速度加成
constexpr float UPHILL_PENALTY = 0.50f;     // 上坡惩罚
constexpr float DOWNHILL_BONUS = 1.20f;     // 下坡加速
