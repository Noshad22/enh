#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

static bool startsWith(const std::string& s, const std::string& prefix) {
  int prefixLength = prefix.length();
  int i = 0;
  while (--prefixLength >= 0) {
    if (s[i] != prefix[i++]) {
      return false;
    }
  }
  return true;
}

static void trimLeft(std::string& s) {
  uint i = 0;
  while (i < s.length() && (s[i] == ' ' || s[i] == '\t')) {
    ++i;
  }
  s.erase(0, i);
}

enum ConditionCode {
  COND_EQ, COND_NE, COND_CS, COND_CC,
  COND_MI, COND_PL, COND_VS, COND_VC,
  COND_HI, COND_LS, COND_GE, COND_LT,
  COND_GT, COND_LE, COND_AL, COND_NV,
};

typedef std::pair<std::string, ConditionCode> Condition;
typedef std::vector<Condition> Conditions;

static const Conditions& getConditions() {
  static Conditions conditions;
  static bool initialized = false;
  if (!initialized) {
    conditions.push_back(std::make_pair("al", COND_AL));
    conditions.push_back(std::make_pair("cc", COND_CC));
    conditions.push_back(std::make_pair("cs", COND_CS));
    conditions.push_back(std::make_pair("eq", COND_EQ));
    conditions.push_back(std::make_pair("ge", COND_GE));
    conditions.push_back(std::make_pair("gt", COND_GT));
    conditions.push_back(std::make_pair("hi", COND_HI));
    conditions.push_back(std::make_pair("ls", COND_LS));
    conditions.push_back(std::make_pair("lt", COND_LT));
    conditions.push_back(std::make_pair("mi", COND_MI));
    conditions.push_back(std::make_pair("ne", COND_NE));
    conditions.push_back(std::make_pair("nv", COND_NV));
    conditions.push_back(std::make_pair("pl", COND_PL));
    conditions.push_back(std::make_pair("vc", COND_VC));
    conditions.push_back(std::make_pair("vs", COND_VS));
    initialized = true;
  }
  return conditions;
}

static int indexOf(const char* s, char ch) {
  for (const char* p = s; *p != 0; ++p) {
    if (*p == ch) {
      return (p - s);
    }
  }
  return -1;
}

/**
 * Parses non-negative integer literals in decimal, hex (starting "0x"),
 * octal (starting "0o"), or binary (starting "0b").
 */
static int parseInt(std::string& s) {
  int base = 10;
  const char* digits = "0123456789";
  if (startsWith(s, "0x")) {
    s.erase(0, 2);
    base = 16;
    digits = "0123456789abcdef";
  } else if (startsWith(s, "0o")) {
    s.erase(0, 2);
    base = 8;
    digits = "01234567";
  } else if (startsWith(s, "0b")) {
    s.erase(0, 2);
    base = 2;
    digits = "01";
  }
  // FIXME: support character literals like 'c', too?
  int result = 0;
  int digit;
  while ((digit = indexOf(digits, s[0])) != -1) {
    result = (result * base) + digit;
    s.erase(0, 1);
  }
  return result;
}

/**
 * Parses a register name, either a numbered name r0-r15, or one of the
 * special names "sp", "lr", or "pc".
 */
static int parseRegister(std::string& s) {
  if (s[0] == 'r') {
    s.erase(0, 1);
    // FIXME: it's a bit weird that you can say r0b1101 or r0xf and so on ;-)
    int reg = parseInt(s);
    if (reg > 15) {
      std::cerr << "register " << reg << " too large!\n";
      exit(EXIT_FAILURE);
    }
    return reg;
  } else if (startsWith(s, "sp")) {
    s.erase(0, 2);
    return 13;
  } else if (startsWith(s, "lr")) {
    s.erase(0, 2);
    return 14;
  } else if (startsWith(s, "pc")) {
    s.erase(0, 2);
    return 15;
  } else {
    std::cerr << "expected register!\n";
    exit(EXIT_FAILURE);
  }
}

enum Mnemonic {
  // FIXME: document that these values are significant.
  OP_AND = 0x0, OP_EOR = 0x1, OP_SUB = 0x2, OP_RSB = 0x3,
  OP_ADD = 0x4, OP_ADC = 0x5, OP_SBC = 0x6, OP_RSC = 0x7,
  OP_TST = 0x8, OP_TEQ = 0x9, OP_CMP = 0xa, OP_CMN = 0xb,
  OP_ORR = 0xc, OP_MOV = 0xd, OP_BIC = 0xe, OP_MVN = 0xf,
  // FIXME: document that these don't matter.
  M_BL, M_B,
  M_MUL, M_MLA,
  M_LDR, M_STR,
  M_SWI,
};

struct Fixup {
  uint32_t address;
  std::string label;
};

class ArmAssembler {
public:
  ArmAssembler(const char* path) : path_(path), lineNumber_(0) {
  }
  
  void assembleFile() {
    std::ifstream in(path_.c_str());
    if (!in) {
      std::cerr << path_ << ": couldn't open: " << strerror(errno) << "\n";
      exit(EXIT_FAILURE);
    }
    
    while (getline(in, line_)) {
      ++lineNumber_;
      originalLine_ = line_;
      
      // Handle labels.
      if (!line_.empty() && isalnum(line_[0])) {
        handleLabel();
      } else {
        // Trim leading whitespace.
        trimLeft(line_);
        if (line_.empty() || line_[0] == '#') {
          // Skip blank or comment lines.
        } else if (line_[0] == '.') {
          handleDirective();
        } else {
          handleInstruction();
        }
      }
    }
    
    fixForwardBranches();
    writeBytes("a.out", &code_[0], &code_[code_.size()]);
  }
  
private:
  int address() {
    return code_.size();
  }
  
  void handleLabel() {
    size_t colon = line_.find(':');
    if (colon == std::string::npos) {
      error("labels must be terminated with ':'");
    }
    std::string label(line_.substr(0, colon));
    if (labels_.find(label) != labels_.end()) {
      error("duplicate definitions of label '" + label + "'");
    }
    labels_[label] = address();
  }
  
  void handleInstruction() {
    instruction_ = 0;
    const Mnemonic mnemonic = parseMnemonic(line_);
    if (mnemonic == OP_ADD || mnemonic == OP_ADC || mnemonic == OP_AND ||
        mnemonic == OP_BIC || mnemonic == OP_EOR || mnemonic == OP_ORR ||
        mnemonic == OP_RSB || mnemonic == OP_RSC || mnemonic == OP_SBC ||
        mnemonic == OP_SUB) {
      // (ADD|ADC|AND|BIC|EOR|ORR|RSB|RSC|SBC|SUB)<cond>S? rd,rn,<rhs>
      parseCondition(line_, 3);
      parseS(line_);
      trimLeft(line_);
      const int rd = parseRegister(line_);
      expectComma(line_);
      const int rn = parseRegister(line_);
      expectComma(line_);
      parseRhs(line_);
      instruction_ |= (mnemonic << 21) | (rn << 16) | (rd << 12);
    } else if (mnemonic == OP_MOV || mnemonic == OP_MVN) {
      // (MOV|MVN)<cond>S? rd,<rhs>
      parseCondition(line_, 3);
      parseS(line_);
      trimLeft(line_);
      const int rd = parseRegister(line_);
      expectComma(line_);
      parseRhs(line_);
      instruction_ |= (mnemonic << 21) | (rd << 12);
    } else if (mnemonic == OP_CMN || mnemonic == OP_CMP || mnemonic == OP_TEQ || mnemonic == OP_TST) {
      // (CMN|CMP|TEQ|TST)<cond>P? rn,<rhs>
      parseCondition(line_, 3);
      // FIXME: P?
      trimLeft(line_);
      const int rn = parseRegister(line_);
      expectComma(line_);
      parseRhs(line_);
      instruction_ |= (mnemonic << 21) | (1 << 20) | (rn << 16);
    } else if (mnemonic == M_B || mnemonic == M_BL) {
      // (B|BL)<cond> label
      if (mnemonic == M_BL) {
        instruction_ |= (1 << 24);
        parseCondition(line_, 2);
      } else {
        parseCondition(line_, 1);
      }
      trimLeft(line_);
      
      Fixup fixup;
      fixup.address = address();
      fixup.label = line_;  // FIXME: should be allowed comments after labels.
      line_.clear();
      
      uint32_t offset = 0;
      if (!resolveLabel(fixup, offset)) {
        fixups_.push_back(fixup);
      }
      
      instruction_ |= (0x5 << 25) | offset;
    } else if (mnemonic == M_MUL || mnemonic == M_MLA) {
      // MUL<cond>S? rd,rm,rs
      // MLA<cond>S? rd,rm,rs,rn
      parseCondition(line_, 3);
      parseS(line_);
      trimLeft(line_);
      const int rd = parseRegister(line_);
      expectComma(line_);
      const int rm = parseRegister(line_);
      expectComma(line_);
      const int rs = parseRegister(line_);
      if (rd == rm) {
        error("destination register and first operand register must differ");
      }
      if (rd == 15 || rs == 15 || rm == 15) {
        error("can't multiply using r15");
      }
      if (mnemonic == M_MLA) {
        expectComma(line_);
        const int rn = parseRegister(line_);
        if (rn == 15) {
          error("can't multiply using r15");
        }
        instruction_ |= (1 << 21) | (rn << 12);
      }
      instruction_ |= (rd << 16) | (rs << 8) | (9 << 4) | (rm << 0);
    } else if (mnemonic == M_LDR || mnemonic == M_STR) {
      // (LDR|STR){cond}{B} <dest>,[<base>{,#<imm>}]{!}
      // (LDR|STR){cond}{B} <dest>,[<base>,{+|-}<off>{,<shift>}]{!}
      // (LDR|STR){cond}{B} <dest>,<expression>
      // (LDR|STR){cond}{B} <dest>,[<base>],#<imm>
      // (LDR|STR){cond}{B} <dest>,[<base>],{+|-}<off>{,<shift>}
      
      // xxxx010P UBWLnnnn ddddoooo oooooooo  Immediate form
      // xxxx011P UBWLnnnn ddddcccc ctt0mmmm  Register form
      parseCondition(line_, 3);
      if (mnemonic == M_LDR) {
        instruction_ |= (0x2 << 24) | (1 << 20);
      } else {
        instruction_ |= (0x3 << 24) | (0 << 20);
      }
      parseSuffixAndSetBit(line_, 'b', (1 << 22));
      // FIXME: P == pre-indexed
      // FIXME: U == positive offset
      // FIXME: W == write-back/translate
      trimLeft(line_);
      const int rd = parseRegister(line_);
      instruction_ |= (rd << 12);
      expectComma(line_);
      // FIXME: implement the other addressing modes.
      expect(line_, '[');
      const int rn = parseRegister(line_);
      instruction_ |= (rn << 16);
      expectComma(line_);
      const int rm = parseRegister(line_);
      instruction_ |= (rm << 0);
      expect(line_, ']');
    } else if (mnemonic == M_SWI) {
      parseCondition(line_, 3);
      trimLeft(line_);
      const int comment = parseInt(line_);
      // FIXME: check 'comment' fits in 24 bits.
      instruction_ |= (0xf << 24) | comment;
    } else {
      error("internal error: unimplemented mnemonic");
    }
    
    /*
     * 
     * (LDM|STM){cond}<type1> <base>{!},<registers>{^}
     * (LDM|STM){cond}<type2> <base>{!},<registers>{^}
     * <type1> is F|E A|D
     * <type2> is I|D B|A
     * <base> is a register
     * <registers> is open-brace comma-separated-list close-brace
     */
    
    // The only thing left should be whitespace or an end-of-line comment.
    trimLeft(line_);
    if (!line_.empty() && line_[0] != '#') {
      error("junk at end of line: '" + line_ + "'");
    }
    
    printf("%4i : 0x%08x : 0x%08x : %s", lineNumber_, address(), instruction_, originalLine_.c_str());
    if (!line_.empty()) {
      printf("    --'%s'", line_.c_str());
    }
    printf("\n");
    
    code_.push_back((instruction_ >> 24) & 0xff);
    code_.push_back((instruction_ >> 16) & 0xff);
    code_.push_back((instruction_ >> 8) & 0xff);
    code_.push_back((instruction_ >> 0) & 0xff);
  }
  
  void handleDirective() {
    error("directive not understood");
  }
  
  // FIXME: throw exceptions and use something like InplaceString.
  void writeBytes(const std::string& path, const uint8_t* begin, const uint8_t* end) {
    int fd = TEMP_FAILURE_RETRY(open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0666));
    if (fd == -1) {
      error("couldn't open output file: " + std::string(strerror(errno)));
    }
    
    const uint8_t* p = begin;
    while (p != end) {
      int bytesWritten = TEMP_FAILURE_RETRY(write(fd, p, end - p));
      if (bytesWritten == -1) {
        error("write failed: " + std::string(strerror(errno)));
        // FIXME: this would leak 'fd' except 'error' actually exits. need scoped_fd.
      }
      std::cerr << "bytes written: " << bytesWritten << "\n";
      p += bytesWritten;
    }
    
    if (TEMP_FAILURE_RETRY(close(fd)) == -1) {
      error("couldn't close output file: " + std::string(strerror(errno)));
    }
  }
  
  Mnemonic parseMnemonic(std::string& s) {
    if (startsWith(s, "and")) {
      return OP_AND;
    } else if (startsWith(s, "eor")) {
      return OP_EOR;
    } else if (startsWith(s, "sub")) {
      return OP_SUB;
    } else if (startsWith(s, "rsb")) {
      return OP_RSB;
    } else if (startsWith(s, "add")) {
      return OP_ADD;
    } else if (startsWith(s, "adc")) {
      return OP_ADC;
    } else if (startsWith(s, "sbc")) {
      return OP_SBC;
    } else if (startsWith(s, "rsc")) {
      return OP_RSC;
    } else if (startsWith(s, "tst")) {
      return OP_TST;
    } else if (startsWith(s, "teq")) {
      return OP_TEQ;
    } else if (startsWith(s, "cmp")) {
      return OP_CMP;
    } else if (startsWith(s, "cmn")) {
      return OP_CMN;
    } else if (startsWith(s, "orr")) {
      return OP_ORR;
    } else if (startsWith(s, "mov")) {
      return OP_MOV;
    } else if (startsWith(s, "bic")) {
      return OP_BIC;
    } else if (startsWith(s, "mvn")) {
      return OP_MVN;
    } else if (startsWith(s, "bl")) {
      return M_BL;
    } else if (startsWith(s, "b")) {
      return M_B;
    } else if (startsWith(s, "mul")) {
      return M_MUL;
    } else if (startsWith(s, "mla")) {
      return M_MLA;
    } else if (startsWith(s, "ldr")) {
      return M_LDR;
    } else if (startsWith(s, "str")) {
      return M_STR;
    } else if (startsWith(s, "swi")) {
      return M_SWI;
    } else {
      error("unknown mnemonic");
    }
  }
  
  void parseCondition(std::string& s, int charsToSkip) {
    s.erase(0, charsToSkip);
    const Conditions& conditions(getConditions());
    int result = COND_AL;
    for (size_t i = 0; i < conditions.size(); ++i) {
      const Condition& cond(conditions[i]);
      if (startsWith(s, cond.first)) {
        s.erase(0, 2);
        result = cond.second;
        break;
      }
    }
    instruction_ |= (result << 28);
  }
  
  // Handles the "s" suffix by setting the S bit in the instruction word,
  // causing the instruction to alter the condition codes.
  void parseS(std::string& s) {
    parseSuffixAndSetBit(s, 's', (1 << 20));
  }
  
  void parseSuffixAndSetBit(std::string& s, char suffix, int mask) {
    if (!s.empty() && s[0] == suffix) {
      instruction_ |= mask;
      s.erase(0, 1);
    }
  }
  
  void parseRhs(std::string& s) {
    if (s.empty()) {
      error("expected immediate or register");
    } else if (isdigit(s[0])) {
      // Immediate.
      // xxxx001a aaaSnnnn ddddrrrr bbbbbbbb
      instruction_ |= (1 << 25);
      // FIXME: translate to value and shift!
      // FIXME: check representable!
      // FIXME: automatically translate negative MOVs/CMPs?
      instruction_ |= parseInt(s);
    } else {
      // Register.
      // xxxx000a aaaSnnnn ddddcccc 0tttmmmm
      instruction_ |= parseRegister(s);
      if (s.empty() || s[0] != ' ') {
        return;
      }
      s.erase(0, 1);
      if (!parseShift(s)) {
        error("expected lsl, lsr, asr, or ror");
      }
      if (s.empty() || s[0] != ' ') {
        error("expected shift constant");
      }
      s.erase(0, 1);
      const int c = parseInt(s);
      if (c > 0xffff) {
        error("shift constant too large");
      }
      instruction_ |= (c << 8);
    }
  }
  
  bool parseShift(std::string& s) {
    static const char* shifts[] = { "lsl", "lsr", "asr", "ror" };
    for (int i = 0; i < 4; ++i) {
      if (startsWith(s, shifts[i])) {
        s.erase(0, 3);
        instruction_ |= (i << 5);
        return true;
      }
    }
    return false;
  }
  
  void expectComma(std::string& s) {
    expect(s, ',');
  }
  
  void expect(std::string& s, char ch) {
    if (s.empty() || s[0] != ch) {
      error("expected '" + std::string(ch, 1) + "'");
    }
    s.erase(0, 1);
  }
  
  bool resolveLabel(const Fixup& fixup, uint32_t& offset) {
    if (labels_.find(fixup.label) == labels_.end()) {
      // If this is a forward branch, and we're not yet in the second pass,
      // this isn't necessarily an error, so let the caller decide what to do.
      return false;
    }
    offset = ((labels_[fixup.label] - fixup.address - 8) / 4);
    offset &= 0x00ffffff;
    return true;
  }
  
  void fixForwardBranches() {
    for (size_t i = 0; i < fixups_.size(); ++i) {
      const Fixup& fixup(fixups_[i]);
      uint32_t offset = 0;
      if (!resolveLabel(fixup, offset)) {
        error("undefined label '" + fixup.label + "'"); // FIXME: which line(s) was it referenced on?
      }
      // Patch the already-generated code.
      code_[fixup.address + 1] = (offset >> 16) & 0xff;
      code_[fixup.address + 2] = (offset >> 8) & 0xff;
      code_[fixup.address + 3] = (offset >> 0) & 0xff;
    }
    std::cerr << "fixups resolved: " << fixups_.size() << "\n";
  }
  
  void error(const std::string& msg) __attribute__((noreturn)) {
    std::cerr << path_ << ":" << lineNumber_ << ": " << msg << std::endl;
    exit(EXIT_FAILURE);
  }
  
  std::string path_;
  
  int lineNumber_;
  std::string line_;
  std::string originalLine_;
  
  typedef std::map<std::string, uint32_t> Labels;
  Labels labels_;
  
  std::vector<Fixup> fixups_;
  
  uint32_t instruction_;
  std::vector<uint8_t> code_;
};

int main(int argc, char* argv[]) {
  for (int i = 1; i < argc; ++i) {
    ArmAssembler assembler(argv[i]);
    assembler.assembleFile();
  }
  return EXIT_SUCCESS;
}