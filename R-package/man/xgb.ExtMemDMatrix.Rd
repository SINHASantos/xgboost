% Generated by roxygen2: do not edit by hand
% Please edit documentation in R/xgb.DMatrix.R
\name{xgb.ExtMemDMatrix}
\alias{xgb.ExtMemDMatrix}
\title{DMatrix from External Data}
\usage{
xgb.ExtMemDMatrix(
  data_iterator,
  cache_prefix = tempdir(),
  missing = NA,
  nthread = NULL
)
}
\arguments{
\item{data_iterator}{A data iterator structure as returned by \code{\link[=xgb.DataIter]{xgb.DataIter()}},
which includes an environment shared between function calls, and functions to access
the data in batches on-demand.}

\item{cache_prefix}{The path of cache file, caller must initialize all the directories in this path.}

\item{missing}{A float value to represents missing values in data.

Note that, while functions like \code{\link[=xgb.DMatrix]{xgb.DMatrix()}} can take a generic \code{NA} and interpret it
correctly for different types like \code{numeric} and \code{integer}, if an \code{NA} value is passed here,
it will not be adapted for different input types.

For example, in R \code{integer} types, missing values are represented by integer number \code{-2147483648}
(since machine 'integer' types do not have an inherent 'NA' value) - hence, if one passes \code{NA},
which is interpreted as a floating-point NaN by \code{\link[=xgb.ExtMemDMatrix]{xgb.ExtMemDMatrix()}} and by
\code{\link[=xgb.QuantileDMatrix.from_iterator]{xgb.QuantileDMatrix.from_iterator()}}, these integer missing values will not be treated as missing.
This should not pose any problem for \code{numeric} types, since they do have an inheret NaN value.}

\item{nthread}{Number of threads used for creating DMatrix.}
}
\value{
An 'xgb.DMatrix' object, with subclass 'xgb.ExtMemDMatrix', in which the data is not
held internally but accessed through the iterator when needed.
}
\description{
Create a special type of XGBoost 'DMatrix' object from external data
supplied by an \code{\link[=xgb.DataIter]{xgb.DataIter()}} object, potentially passed in batches from a
bigger set that might not fit entirely in memory.

The data supplied by the iterator is accessed on-demand as needed, multiple times,
without being concatenated, but note that fields like 'label' \strong{will} be
concatenated from multiple calls to the data iterator.

For more information, see the guide 'Using XGBoost External Memory Version':
\url{https://xgboost.readthedocs.io/en/stable/tutorials/external_memory.html}
}
\details{
Be aware that construction of external data DMatrices \bold{will cache data on disk}
in a compressed format, under the path supplied in \code{cache_prefix}.

External data is not supported for the exact tree method.
}
\examples{
data(mtcars)

# This custom environment will be passed to the iterator
# functions at each call. It is up to the user to keep
# track of the iteration number in this environment.
iterator_env <- as.environment(
  list(
    iter = 0,
    x = mtcars[, -1],
    y = mtcars[, 1]
  )
)

# Data is passed in two batches.
# In this example, batches are obtained by subsetting the 'x' variable.
# This is not advantageous to do, since the data is already loaded in memory
# and can be passed in full in one go, but there can be situations in which
# only a subset of the data will fit in the computer's memory, and it can
# be loaded in batches that are accessed one-at-a-time only.
iterator_next <- function(iterator_env) {
  curr_iter <- iterator_env[["iter"]]
  if (curr_iter >= 2) {
    # there are only two batches, so this signals end of the stream
    return(NULL)
  }

  if (curr_iter == 0) {
    x_batch <- iterator_env[["x"]][1:16, ]
    y_batch <- iterator_env[["y"]][1:16]
  } else {
    x_batch <- iterator_env[["x"]][17:32, ]
    y_batch <- iterator_env[["y"]][17:32]
  }
  on.exit({
    iterator_env[["iter"]] <- curr_iter + 1
  })

  # Function 'xgb.DataBatch' must be called manually
  # at each batch with all the appropriate attributes,
  # such as feature names and feature types.
  return(xgb.DataBatch(data = x_batch, label = y_batch))
}

# This moves the iterator back to its beginning
iterator_reset <- function(iterator_env) {
  iterator_env[["iter"]] <- 0
}

data_iterator <- xgb.DataIter(
  env = iterator_env,
  f_next = iterator_next,
  f_reset = iterator_reset
)
cache_prefix <- tempdir()

# DMatrix will be constructed from the iterator's batches
dm <- xgb.ExtMemDMatrix(data_iterator, cache_prefix, nthread = 1)

# After construction, can be used as a regular DMatrix
params <- xgb.params(nthread = 1, objective = "reg:squarederror")
model <- xgb.train(data = dm, nrounds = 2, params = params)

# Predictions can also be called on it, and should be the same
# as if the data were passed differently.
pred_dm <- predict(model, dm)
pred_mat <- predict(model, as.matrix(mtcars[, -1]))
}
\seealso{
\code{\link[=xgb.DataIter]{xgb.DataIter()}}, \code{\link[=xgb.DataBatch]{xgb.DataBatch()}}, \code{\link[=xgb.QuantileDMatrix.from_iterator]{xgb.QuantileDMatrix.from_iterator()}}
}
