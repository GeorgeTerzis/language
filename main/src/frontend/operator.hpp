enum class op_type_e : std::int8_t { UNARY = 1, BINARY = 2, TERNARY = 3 };
enum class op_assoc_e : std::int8_t { LEFT, RIGHT };

#define BOP_ENUM_LIST                                                          \
    bop_first = 1, DIAMOND, PLUS, MINUS, MULT, DIV, MOD, EQ, NEQ, LESS, LEQ,   \
    GEQ, GREATER, AND, XOR, OR, SLEFT, SRIGHT, ASSIGN, PLUSASSIGN,             \
    MINUSASSIGN, MULTASSIGN, DIVASSIGN, PIPE, bop_end
#define UOP_ENUM_LIST                                                          \
    uop_begin = 100, PLUSPLUS, MINUSMINUS, NOT, POSITIVE, NEGATIVE, DEREF,     \
    ADDRESS, AS, uop_end

enum class op_operation_e : std::int8_t {
    first = 0,
    BOP_ENUM_LIST,
    UOP_ENUM_LIST,
    last
};
enum class bop_e : std::int8_t { BOP_ENUM_LIST };
enum class uop_e : std::int8_t { UOP_ENUM_LIST };

enum class op_pos_e : std::int8_t {
    PREFIX,
    INFIX,
    POSTFIX,
};

inline auto str(op_pos_e t) {
    switch (t) {
        case op_pos_e::PREFIX:
            return "PREFIX";
        case op_pos_e::INFIX:
            return "INFIX";
        case op_pos_e::POSTFIX:
            return "POSTFIX";
        default:
            return "UNKNOWN_OP_TYPE";
    }
}
inline auto str(op_type_e t) {
    switch (t) {
        case op_type_e::UNARY:
            return "UNARY";
        case op_type_e::BINARY:
            return "BINARY";
        case op_type_e::TERNARY:
            return "TERNARY";
        default:
            return "UNKNOWN_OP_TYPE";
    }
}

inline auto str(op_assoc_e a) {
    switch (a) {
        case op_assoc_e::LEFT:
            return "LEFT";
        case op_assoc_e::RIGHT:
            return "RIGHT";
        default:
            return "UNKNOWN_OP_ASSOC";
    }
}

inline auto str(op_operation_e o) {
    switch (o) {
        case op_operation_e::PLUS:
            return "PLUS";
        case op_operation_e::MINUS:
            return "MINUS";
        case op_operation_e::MULT:
            return "MULT";
        case op_operation_e::DIV:
            return "DIV";
        case op_operation_e::MOD:
            return "MOD";
        case op_operation_e::PLUSPLUS:
            return "PLUSPLUS";
        case op_operation_e::MINUSMINUS:
            return "MINUSMINUS";
        case op_operation_e::EQ:
            return "EQ";
        case op_operation_e::NEQ:
            return "NEQ";
        case op_operation_e::GREATER:
            return "GREATER";
        case op_operation_e::LESS:
            return "LESS";
        case op_operation_e::LEQ:
            return "LEQ";
        case op_operation_e::GEQ:
            return "GEQ";
        case op_operation_e::NOT:
            return "NOT";
        case op_operation_e::AND:
            return "AND";
        case op_operation_e::XOR:
            return "XOR";
        case op_operation_e::OR:
            return "OR";
        case op_operation_e::SLEFT:
            return "SLEFT";
        case op_operation_e::SRIGHT:
            return "SRIGHT";
        case op_operation_e::ASSIGN:
            return "ASSIGN";
        case op_operation_e::PLUSASSIGN:
            return "PLUSASSIGN";
        case op_operation_e::MINUSASSIGN:
            return "MINUSASSIGN";
        case op_operation_e::MULTASSIGN:
            return "MULTASSIGN";
        case op_operation_e::DIVASSIGN:
            return "DIVASSIGN";
        case op_operation_e::PIPE:
            return "PIPE";
        case op_operation_e::DIAMOND:
            return "DIAMOND";
        case op_operation_e::POSITIVE:
            return "POSITIVE";
        case op_operation_e::NEGATIVE:
            return "NEG";
        case op_operation_e::DEREF:
            return "DEREF";
        case op_operation_e::ADDRESS:
            return "ADDRESS";
        case op_operation_e::AS:
            return "AS";
        case op_operation_e::last:
            return "last";
        default:
            return "UNKNOWN_OP_OPERATION";
    }
}

struct op_meta_t {
    op_operation_e op;
    op_type_e type;
    op_assoc_e assoc;
    op_pos_e pos;
    std::int32_t prec;
    bool has_payload;

    // Compare against op_pos_e
    bool operator==(op_pos_e other) const { return pos == other; }

    // Compare against op_assoc_e
    bool operator==(op_assoc_e other) const { return assoc == other; }

    // Compare against int32_t for precedence
    bool operator==(std::int32_t other) const { return prec == other; }

    // Compare against op_type_e
    bool operator==(op_type_e other) const { return type == other; }

    // Compare against op_operation_e
    bool operator==(op_operation_e other) const { return op == other; }
};

inline std::string str(const op_meta_t& meta) {
    return "{ op: " + std::string(str(meta.op))
           + ", type: " + std::string(str(meta.type))
           + ", assoc: " + std::string(str(meta.assoc)) + ", pos: "
           + std::string(str(meta.pos)) + ", prec: " + std::to_string(meta.prec)
           + ", has_payload: " + (meta.has_payload ? "true" : "false") + " }";
}

#define entry(OPCODE, TYPE, ASSOC, POS, PREC, HAS_PAYLOAD)                     \
    {                                                                          \
        op_operation_e{OPCODE}, op_meta_t {                                    \
            OPCODE, TYPE, ASSOC, POS, PREC, HAS_PAYLOAD                        \
        }                                                                      \
    }

//Precedence is only there for the binary operators which means only the prefix
constexpr static auto op_table =
    frozen::unordered_map<op_operation_e, op_meta_t, 31>{
        entry(op_operation_e::DEREF,
              op_type_e::UNARY,
              op_assoc_e::RIGHT,
              op_pos_e::PREFIX,
              999,
              false),
        entry(op_operation_e::MINUSMINUS,
              op_type_e::UNARY,
              op_assoc_e::RIGHT,
              op_pos_e::POSTFIX,
              999,
              false),
        entry(op_operation_e::PLUSPLUS,
              op_type_e::UNARY,
              op_assoc_e::RIGHT,
              op_pos_e::POSTFIX,
              999,
              false),
        entry(op_operation_e::GREATER,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              60,
              false),
        entry(op_operation_e::PLUS,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              80,
              false),
        entry(op_operation_e::ASSIGN,
              op_type_e::BINARY,
              op_assoc_e::RIGHT,
              op_pos_e::INFIX,
              -99,
              false),
        entry(op_operation_e::DIAMOND,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              60,
              false),
        entry(op_operation_e::MULT,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              90,
              false),
        entry(op_operation_e::OR,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              20,
              false),
        entry(op_operation_e::MINUS,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              80,
              false),
        entry(op_operation_e::MULTASSIGN,
              op_type_e::BINARY,
              op_assoc_e::RIGHT,
              op_pos_e::INFIX,
              -99,
              false),
        entry(op_operation_e::AND,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              40,
              false),
        entry(op_operation_e::LEQ,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              60,
              false),
        entry(op_operation_e::PIPE,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              0,
              false),
        entry(op_operation_e::LESS,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              60,
              false),
        entry(op_operation_e::MINUSASSIGN,
              op_type_e::BINARY,
              op_assoc_e::RIGHT,
              op_pos_e::INFIX,
              -99,
              false),
        entry(op_operation_e::SLEFT,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              70,
              false),
        entry(op_operation_e::XOR,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              30,
              false),
        entry(op_operation_e::POSITIVE,
              op_type_e::UNARY,
              op_assoc_e::LEFT,
              op_pos_e::PREFIX,
              999,
              false),
        entry(op_operation_e::ADDRESS,
              op_type_e::UNARY,
              op_assoc_e::LEFT,
              op_pos_e::PREFIX,
              999,
              false),
        entry(op_operation_e::NOT,
              op_type_e::UNARY,
              op_assoc_e::LEFT,
              op_pos_e::PREFIX,
              999,
              false),
        entry(op_operation_e::AS,
              op_type_e::UNARY,
              op_assoc_e::LEFT,
              op_pos_e::PREFIX,
              999,
              true),
        entry(op_operation_e::MOD,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              90,
              false),
        entry(op_operation_e::EQ,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              50,
              false),
        entry(op_operation_e::GEQ,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              60,
              false),
        entry(op_operation_e::DIVASSIGN,
              op_type_e::BINARY,
              op_assoc_e::RIGHT,
              op_pos_e::INFIX,
              -99,
              false),
        entry(op_operation_e::SRIGHT,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              70,
              false),
        entry(op_operation_e::PLUSASSIGN,
              op_type_e::BINARY,
              op_assoc_e::RIGHT,
              op_pos_e::INFIX,
              -99,
              false),
        entry(op_operation_e::NEQ,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              50,
              false),
        entry(op_operation_e::NEGATIVE,
              op_type_e::UNARY,
              op_assoc_e::LEFT,
              op_pos_e::PREFIX,
              999,
              false),
        entry(op_operation_e::DIV,
              op_type_e::BINARY,
              op_assoc_e::LEFT,
              op_pos_e::INFIX,
              90,
              false),
    };

#undef entry

inline auto left_bp(const op_meta_t& meta) { return meta.prec; }
inline auto right_bp(const op_meta_t& meta) {
    return meta.assoc == op_assoc_e::LEFT ? meta.prec + 1 : meta.prec;
}
