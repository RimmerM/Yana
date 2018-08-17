Prism.languages.yana = {
	'comment': {
		pattern: /(^|[^-!#$%*+=?&@|~.:<>^\\\/])(?:--[^-!#$%*+=?&@|~.:<>^\\\/].*|{-[\s\S]*?-})/m,
		lookbehind: true
	},
	'string': {
		pattern: /(["'])(?:\\(?:\r\n|[\s\S])|(?!\1)[^\\\r\n])*\1/,
    greedy: true,
    inside: {
      interpolation: {
        pattern: /\{[^}]+\}/,
        alias: 'variable',
        inside: {
          delimiter: {
            pattern: /^\{|\}$/,
            alias: 'variable'
          }
          // More at bottom
        }
      }
    },
  },
  'annotation': {
    pattern: /\B@(?:\w+:)?(?:[A-Z]\w*|\[[^\]]+\])/,
    alias: 'builtin'
  },
	'keyword' : /\b(?:fn|match|for|while|class|deriving|do|else|if|in|infixl|infixr|instance|let|module|then|alias|where)\b/,
	'import_statement' : {
		pattern: /((?:\r?\n|\r|^)\s*)import\s+(?:qualified\s+)?(?:[A-Z][\w']*)(?:\.[A-Z][\w']*)*(?:\s+as\s+(?:[A-Z][_a-zA-Z0-9']*)(?:\.[A-Z][\w']*)*)?(?:\s+hiding\b)?/m,
		lookbehind: true,
		inside: {
			'keyword': /\b(?:import|qualified|as|hiding)\b/
		}
  },
  'record_decl' : {
		pattern: /((?:\r?\n|\r|^)\s*)data\s+(?:qualified\s+)?/m,
		lookbehind: true,
		inside: {
			'keyword': /\b(?:data|qualified)\b/
		}
	},
	'builtin': /\b(?:abs|acos|acosh|all|and|any|appendFile|approxRational|asTypeOf|asin|asinh|atan|atan2|atanh|basicIORun|break|catch|ceiling|chr|compare|concat|concatMap|const|cos|cosh|curry|cycle|decodeFloat|denominator|digitToInt|div|divMod|drop|dropWhile|either|elem|encodeFloat|enumFrom|enumFromThen|enumFromThenTo|enumFromTo|error|even|exp|exponent|fail|filter|flip|floatDigits|floatRadix|floatRange|floor|fmap|foldl|foldl1|foldr|foldr1|fromDouble|fromEnum|fromInt|fromInteger|fromIntegral|fromRational|fst|gcd|getChar|getContents|getLine|group|head|id|inRange|index|init|intToDigit|interact|ioError|isAlpha|isAlphaNum|isAscii|isControl|isDenormalized|isDigit|isHexDigit|isIEEE|isInfinite|isLower|isNaN|isNegativeZero|isOctDigit|isPrint|isSpace|isUpper|iterate|last|lcm|length|lex|lexDigits|lexLitChar|lines|log|logBase|lookup|map|mapM|mapM_|max|maxBound|maximum|maybe|min|minBound|minimum|mod|negate|not|notElem|null|numerator|odd|or|ord|otherwise|pack|pi|pred|primExitWith|print|product|properFraction|putChar|putStr|putStrLn|quot|quotRem|range|rangeSize|read|readDec|readFile|readFloat|readHex|readIO|readInt|readList|readLitChar|readLn|readOct|readParen|readSigned|reads|readsPrec|realToFrac|recip|rem|repeat|replicate|return|reverse|round|scaleFloat|scanl|scanl1|scanr|scanr1|seq|sequence|sequence_|show|showChar|showInt|showList|showLitChar|showParen|showSigned|showString|shows|showsPrec|significand|signum|sin|sinh|snd|sort|span|splitAt|sqrt|subtract|succ|sum|tail|take|takeWhile|tan|tanh|threadToIOResult|toEnum|toInt|toInteger|toLower|toRational|toUpper|truncate|uncurry|undefined|unlines|until|unwords|unzip|unzip3|userError|words|writeFile|zip|zip3|zipWith|zipWith3)\b/,
	'number': /\b(?:0[xX][\da-fA-F]+(?:_[\da-fA-F]+)*|0[bB][01]+(?:_[01]+)*|\d+(?:_\d+)*(?:\.\d+(?:_\d+)*)?(?:[eE][+-]?\d+(?:_\d+)*)?[fFL]?)\b/,
  'boolean': /\b(?:true|false)\b/,
	'operator' : /\s\.\s|[-!#$%*+=?&@|~.:<>^\\\/]*\.[-!#$%*+=?&@|~.:<>^\\\/]+|[-!#$%*+=?&@|~.:<>^\\\/]+\.[-!#$%*+=?&@|~.:<>^\\\/]*|[-!#$%*+=?&@|~:<>^\\\/]+|`([A-Z][\w']*\.)*[_a-z][\w']*`/,
	'hvariable': /\b(?:[A-Z][\w']*\.)*[_a-z][\w']*\b/,
	'constant': /\b(?:[A-Z][\w']*\.)*[A-Z][\w']*\b/,
	'punctuation' : /[{}[\];(),.:]/
};

Prism.languages.yana.string.inside.interpolation.inside.rest = Prism.languages.yana;