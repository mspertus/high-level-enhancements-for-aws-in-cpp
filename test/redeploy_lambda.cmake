# Best-effort delete of any existing function (first run will have nothing to delete).
execute_process(COMMAND aws lambda delete-function --function-name ${LAMBDA_NAME})

execute_process(
    COMMAND aws lambda create-function
        --function-name ${LAMBDA_NAME}
        --role ${LAMBDA_EXECUTION_ROLE}
        --runtime provided.al2023
        --timeout 15
        --memory-size 128
        --handler ${LAMBDA_NAME}
        --zip-file fileb://${LAMBDA_NAME}.zip
    COMMAND_ERROR_IS_FATAL ANY)

# CreateFunction returns before the function is invocable; the function sits
# in State=Pending while Lambda provisions the container. Wait for it to
# become Active so the downstream invocation tests don't race.
execute_process(
    COMMAND aws lambda wait function-active --function-name ${LAMBDA_NAME}
    COMMAND_ERROR_IS_FATAL ANY)