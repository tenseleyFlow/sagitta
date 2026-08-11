module Demo.Core where
import qualified Data.Map as Map

data Choice = Yes | No deriving (Show)
answer x = if x >= 1.5e2 then True else False
compose = fmap (+ 1) . Map.lookup "key\n"; pair = (Yes, No)
letter = '\t'
symbols a b = a >>= b <> a $ b
