{-# LANGUAGE TupleSections #-}

module QLang.FDescr where

import Language.Haskell.Interpreter
import Network.PFq.Default
import Network.PFq.Experimental
import Network.PFq.Lang as Q

import qualified Data.Aeson as A
import Control.Exception
import Data.Either
import qualified Data.ByteString.Lazy.Char8 as C

import QLang.Util
import Control.Monad.Trans.Reader
import Control.Monad(when)
import Options


compile :: String -> OptionT IO String
compile raw = do
    let (code, localImports) = parseCode raw
    opt <- ask
    imports <- mkImportList localImports
    lift $ do
      when (verb opt > 0) (putStrLn ("imports: " ++ show imports))
      res <- runInterpreter $ do
          setImportsQ imports
          interpret (mkMainFunction code) (as :: (Function (SkBuff -> Action SkBuff)))
      either throw (\comp -> return (show (fst $ Q.serialize comp 0)))  res


